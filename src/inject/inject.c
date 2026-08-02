// libprocwatch_inject.so - LD_PRELOAD auto-instrumentation decider.
//
// This library is loaded into every process that inherits LD_PRELOAD, so its
// only job is to decide and to set environment variables. The actual
// instrumentation is loaded afterwards by the runtime's own supported hook
// (-javaagent for the JVM, sitecustomize for CPython). Keeping the heavy
// lifting out of here means a bug in an agent degrades one application
// instead of bricking every process on the host.
//
// Constraints that follow from that, and that must not be relaxed:
//   - No undefined symbols, enforced at link time with --no-undefined. A
//     preloaded object that loads but cannot resolve a symbol aborts the
//     process with a relocation error on both glibc and musl (measured:
//     exit 127 for every command, including the shell you would use to
//     repair it). By contrast, an object that cannot be loaded at all is
//     merely warned about and ignored, so failing to load is safe and
//     failing to relocate is not.
//   - libc is the only DT_NEEDED, which keeps the set of symbols that could
//     fail to resolve as small as it can be.
//   - Nothing is exported. A stray exported getenv or malloc would replace
//     the real one process-wide.
//   - No dlopen: we are inside the loader's init phase.
//   - Every failure is swallowed. No assert, no abort, no exit.

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>

#include "../../include/inject.h"

#ifndef PW_DEFAULT_AGENT_DIR
#define PW_DEFAULT_AGENT_DIR "/opt/procwatch/agent"
#endif

#define PW_DEFAULT_ENDPOINT "http://127.0.0.1:4318"

static int g_debug = 0;

void pw_debug(const char *fmt, ...) {
    if (!g_debug) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("procwatch-inject: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

const char *pw_agent_dir(void) {
    const char *dir = getenv("PROCWATCH_AGENT_DIR");
    return (dir && *dir) ? dir : PW_DEFAULT_AGENT_DIR;
}

static int path_exists(const char *path) {
    return access(path, F_OK) == 0;
}

static const char *exec_basename(void) {
    const char *execfn = (const char *)getauxval(AT_EXECFN);
    if (!execfn || !*execfn) return NULL;
    const char *slash = strrchr(execfn, '/');
    return slash ? slash + 1 : execfn;
}

// Service name precedence: an explicit OTEL_SERVICE_NAME wins, then
// PROCWATCH_SERVICE, then the executable basename so that something useful
// still lands in the database when nothing was configured.
static const char *resolve_service_name(void) {
    const char *name = getenv("OTEL_SERVICE_NAME");
    if (name && *name) return name;
    name = getenv("PROCWATCH_SERVICE");
    if (name && *name) return name;
    name = exec_basename();
    return (name && *name) ? name : "unknown_service";
}

void pw_apply_common(void) {
    const char *service = resolve_service_name();
    pw_env_set_default("OTEL_SERVICE_NAME", service);

    // The collector reads this back out of /proc/<pid>/environ to label
    // process metrics with the same service name the spans carry.
    setenv("PROCWATCH_SERVICE", service, 1);

    const char *endpoint = getenv("PROCWATCH_ENDPOINT");
    if (!endpoint || !*endpoint) endpoint = PW_DEFAULT_ENDPOINT;
    pw_env_set_default("OTEL_EXPORTER_OTLP_ENDPOINT", endpoint);

    // procwatch-agentd speaks OTLP over HTTP only; gRPC would need a second
    // server and HTTP/2 framing for no benefit here.
    pw_env_set_default("OTEL_EXPORTER_OTLP_PROTOCOL", "http/protobuf");
    pw_env_set_default("OTEL_TRACES_EXPORTER", "otlp");
    pw_env_set_default("OTEL_METRICS_EXPORTER", "otlp");
    pw_env_set_default("OTEL_LOGS_EXPORTER", "otlp");
}

void pw_apply_java(const char *agent_dir) {
    char jar[1024];
    snprintf(jar, sizeof jar, "%s/java/javaagent.jar", agent_dir);

    // A -javaagent pointing at a missing jar aborts JVM startup outright, so
    // an unmounted payload would take the application down. Checking first
    // turns that from an outage into a no-op.
    if (!path_exists(jar)) {
        pw_debug("no java agent at %s, skipping", jar);
        return;
    }

    char opt[1152];
    // Leading space, and append rather than overwrite: the JVMTI spec asks
    // tools to share JAVA_TOOL_OPTIONS rather than claim it.
    snprintf(opt, sizeof opt, " -javaagent:%s", jar);

    if (pw_env_append("JAVA_TOOL_OPTIONS", opt, jar) == 0) {
        pw_debug("injected java agent %s", jar);
    }
}

void pw_apply_python(const char *agent_dir) {
    char shim_dir[1024];
    snprintf(shim_dir, sizeof shim_dir, "%s/python", agent_dir);

    char shim[1088];
    snprintf(shim, sizeof shim, "%s/sitecustomize.py", shim_dir);
    if (!path_exists(shim)) {
        pw_debug("no python shim at %s, skipping", shim);
        return;
    }

    // Prepended so our sitecustomize shadows any other one on the path; site
    // imports the first it finds.
    if (pw_env_prepend("PYTHONPATH", shim_dir, shim_dir, ':') == 0) {
        pw_debug("injected python shim %s", shim_dir);
    }
}

__attribute__((constructor))
static void pw_init(void) {
    // Guard against a second constructor run inside one image. Re-exec still
    // gets a fresh copy, which is why the env helpers are idempotent too.
    static int applied = 0;
    if (applied) return;
    applied = 1;

    g_debug = getenv("PROCWATCH_INJECT_DEBUG") != NULL;

    const char *off = getenv("PROCWATCH_INJECT_DISABLED");
    if (off && *off && strcmp(off, "0") != 0) {
        pw_debug("disabled by PROCWATCH_INJECT_DISABLED");
        return;
    }

    // The loader already drops LD_PRELOAD under AT_SECURE, and HotSpot
    // independently ignores JAVA_TOOL_OPTIONS when privileges differ. Bail
    // out explicitly rather than half-configuring a process that will not
    // honour the result.
    if (geteuid() != getuid() || getegid() != getgid()) {
        pw_debug("privileged process, skipping");
        return;
    }

    pw_runtime_t rt = pw_detect_by_name();
    if (rt == PW_RT_UNKNOWN) return;

    rt = pw_confirm_by_symbol(rt);
    if (rt == PW_RT_UNKNOWN) return;

    const char *agent_dir = pw_agent_dir();

    switch (rt) {
        case PW_RT_JAVA:
            pw_apply_common();
            pw_apply_java(agent_dir);
            break;
        case PW_RT_PYTHON:
            pw_apply_common();
            pw_apply_python(agent_dir);
            break;
        default:
            break;
    }
}
