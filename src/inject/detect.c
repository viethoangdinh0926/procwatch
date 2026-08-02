// Runtime detection for the LD_PRELOAD injector.
//
// Everything here runs inside an ELF constructor, before main(), in every
// process that inherits LD_PRELOAD. It must be cheap for the common case
// (a shell or a coreutil, which we reject on the basename alone) and must
// never allocate, dlopen, or fail loudly.

#define _GNU_SOURCE
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/auxv.h>

#include "../../include/inject.h"

// Declared weak so the library keeps libc as its only DT_NEEDED. glibc 2.34+
// and musl both provide dlsym from libc proper; on older glibc a process that
// never linked libdl simply leaves this NULL and we fall back to the
// name-based verdict rather than failing to load.
extern void *dlsym(void *handle, const char *symbol) __attribute__((weak));

#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)0)
#endif

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// "python3.12" and "python3" both count; "python-config" does not.
static int is_python_name(const char *name) {
    if (strncmp(name, "python", 6) != 0) return 0;
    for (const char *p = name + 6; *p; ++p) {
        if ((*p < '0' || *p > '9') && *p != '.') return 0;
    }
    return 1;
}

static pw_runtime_t classify_name(const char *name) {
    if (strcmp(name, "java") == 0) return PW_RT_JAVA;
    if (is_python_name(name)) return PW_RT_PYTHON;
    // Common Python application servers exec'd directly rather than via the
    // interpreter's own basename.
    if (strcmp(name, "gunicorn") == 0 ||
        strcmp(name, "uwsgi") == 0 ||
        strcmp(name, "celery") == 0 ||
        strcmp(name, "uvicorn") == 0 ||
        strcmp(name, "flask") == 0) {
        return PW_RT_PYTHON;
    }
    return PW_RT_UNKNOWN;
}

// AT_EXECFN costs no syscalls but holds the path as passed to execve, which
// may be relative or a wrapper. /proc/self/cmdline is the fallback and also
// covers the case where the auxv entry is absent.
static int read_cmdline_argv0(char *buf, size_t bufsz) {
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return 0;
}

pw_runtime_t pw_detect_by_name(void) {
    pw_runtime_t rt = PW_RT_UNKNOWN;

    const char *execfn = (const char *)getauxval(AT_EXECFN);
    if (execfn && *execfn) {
        rt = classify_name(basename_of(execfn));
        if (rt != PW_RT_UNKNOWN) return rt;
    }

    char cmdline[512];
    if (read_cmdline_argv0(cmdline, sizeof cmdline) == 0 && cmdline[0]) {
        rt = classify_name(basename_of(cmdline));
        if (rt != PW_RT_UNKNOWN) return rt;
    }

    // A wrapper script may exec the real runtime later; that new image gets
    // its own constructor run, so there is nothing to do here.
    return PW_RT_UNKNOWN;
}

pw_runtime_t pw_confirm_by_symbol(pw_runtime_t hint) {
    if (!dlsym) return hint;

    if (hint == PW_RT_JAVA) {
        // Probe JLI_Launch, not JNI_CreateJavaVM. The `java` launcher links
        // only libjli.so and dlopens libjvm.so later, so looking for the VM
        // itself at constructor time yields a false negative.
        if (dlsym(RTLD_DEFAULT, "JLI_Launch")) return PW_RT_JAVA;
        // An embedded JVM links libjvm directly and does export this.
        if (dlsym(RTLD_DEFAULT, "JNI_CreateJavaVM")) return PW_RT_JAVA;
        return hint;
    }

    if (hint == PW_RT_PYTHON) {
        // CPython links its main program with --export-dynamic so interpreter
        // symbols resolve for extension modules. That holds whether libpython
        // is static (Debian's /usr/bin/python3.12) or shared (the official
        // python:3.x images).
        if (dlsym(RTLD_DEFAULT, "Py_BytesMain")) return PW_RT_PYTHON;
        if (dlsym(RTLD_DEFAULT, "Py_Initialize")) return PW_RT_PYTHON;
        return hint;
    }

    return PW_RT_UNKNOWN;
}
