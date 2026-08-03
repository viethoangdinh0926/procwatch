// libprocwatch_inject.so - LD_PRELOAD auto-instrumentation decider + metric
// thread bootstrap.
//
// Constraints that must not be relaxed:
//   - No undefined symbols (--no-undefined). A preloaded object that loads
//     but cannot resolve a symbol aborts the process on both glibc and musl.
//   - libc is the only DT_NEEDED.
//   - Export only the interposed symbols (__libc_start_main, execve, ...).
//   - No dlopen from the constructor.
//   - Every failure is swallowed.

#define _GNU_SOURCE
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>

#include "../../include/inject.h"
#include "../../include/proc_push.h"

#ifndef PW_DEFAULT_AGENT_DIR
#define PW_DEFAULT_AGENT_DIR "/opt/procwatch/agent"
#endif

#define PW_DEFAULT_ENDPOINT "http://127.0.0.1:4318"

static int g_debug = 0;
static int g_armed = 0;
static const char *g_runtime_hint = "other";

void pw_debug(const char *fmt, ...) {
    if (!g_debug) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("procwatch-inject: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int pw_injection_armed(void) { return g_armed; }
void pw_set_injection_armed(int armed) { g_armed = armed; }
const char *pw_runtime_hint(void) { return g_runtime_hint; }
void pw_set_runtime_hint(const char *hint) {
    g_runtime_hint = (hint && *hint) ? hint : "other";
}

const char *pw_agent_dir(void) {
    const char *dir = getenv("PROCWATCH_AGENT_DIR");
    return (dir && *dir) ? dir : PW_DEFAULT_AGENT_DIR;
}

const char *pw_resolve_label(void) {
    const char *label = getenv("PROCWATCH_LABEL");
    if (!pw_label_valid(label)) return NULL;
    return label;
}

void pw_attach_label_to_otel(const char *label) {
    if (!label || !*label) return;
    char attr[160];
    snprintf(attr, sizeof attr, "procwatch.label=%s", label);
    const char *cur = getenv("OTEL_RESOURCE_ATTRIBUTES");
    if (cur && strstr(cur, "procwatch.label=")) return;
    if (!cur || !*cur) {
        setenv("OTEL_RESOURCE_ATTRIBUTES", attr, 1);
        return;
    }
    char buf[8192];
    if (strlen(cur) + 1 + strlen(attr) + 1 > sizeof buf) return;
    snprintf(buf, sizeof buf, "%s,%s", cur, attr);
    setenv("OTEL_RESOURCE_ATTRIBUTES", buf, 1);
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

static const char *resolve_service_name(void) {
    const char *name = getenv("OTEL_SERVICE_NAME");
    if (name && *name) return name;
    name = getenv("PROCWATCH_SERVICE");
    if (name && *name) return name;
    name = exec_basename();
    return (name && *name) ? name : "unknown_service";
}

void pw_apply_common(void) {
    const char *label = pw_resolve_label();
    if (label) pw_attach_label_to_otel(label);

    const char *service = resolve_service_name();
    pw_env_set_default("OTEL_SERVICE_NAME", service);
    setenv("PROCWATCH_SERVICE", service, 1);

    const char *endpoint = getenv("PROCWATCH_ENDPOINT");
    if (!endpoint || !*endpoint) endpoint = PW_DEFAULT_ENDPOINT;
    pw_env_set_default("OTEL_EXPORTER_OTLP_ENDPOINT", endpoint);

    pw_env_set_default("OTEL_EXPORTER_OTLP_PROTOCOL", "http/protobuf");
    pw_env_set_default("OTEL_TRACES_EXPORTER", "otlp");
    pw_env_set_default("OTEL_METRICS_EXPORTER", "otlp");
    pw_env_set_default("OTEL_LOGS_EXPORTER", "otlp");
}

void pw_apply_java(const char *agent_dir) {
    char jar[1024];
    snprintf(jar, sizeof jar, "%s/java/javaagent.jar", agent_dir);
    if (!path_exists(jar)) {
        pw_debug("no java agent at %s, skipping", jar);
        return;
    }
    char opt[1152];
    snprintf(opt, sizeof opt, " -javaagent:%s", jar);
    if (pw_env_append("JAVA_TOOL_OPTIONS", opt, jar) == 0)
        pw_debug("injected java agent %s", jar);
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
    if (pw_env_prepend("PYTHONPATH", shim_dir, shim_dir, ':') == 0)
        pw_debug("injected python shim %s", shim_dir);
}

__attribute__((constructor))
static void pw_init(void) {
    static int applied = 0;
    if (applied) return;
    applied = 1;

    g_debug = getenv("PROCWATCH_INJECT_DEBUG") != NULL;

    const char *off = getenv("PROCWATCH_INJECT_DISABLED");
    if (off && *off && strcmp(off, "0") != 0) {
        pw_debug("disabled by PROCWATCH_INJECT_DISABLED");
        return;
    }

    if (geteuid() != getuid() || getegid() != getgid()) {
        pw_debug("privileged process, skipping");
        return;
    }

    if (!pw_resolve_label()) {
        pw_debug("PROCWATCH_LABEL missing or invalid; skipping injection");
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
            pw_set_runtime_hint("java");
            pw_set_injection_armed(1);
            break;
        case PW_RT_PYTHON:
            pw_apply_common();
            pw_apply_python(agent_dir);
            pw_set_runtime_hint("python");
            pw_set_injection_armed(1);
            break;
        default:
            break;
    }
}
