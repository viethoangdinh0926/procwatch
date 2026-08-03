#pragma once

#include <stddef.h>

// Runtime kinds the injector knows how to bootstrap.
typedef enum {
    PW_RT_UNKNOWN = 0,
    PW_RT_JAVA,
    PW_RT_PYTHON
} pw_runtime_t;

// Cheap first-pass filter: matches the executable basename against the small
// set of names worth probing further. Returns PW_RT_UNKNOWN for everything
// else so that shells and coreutils pay only the cost of loading the library.
pw_runtime_t pw_detect_by_name(void);

// Second-pass confirmation via dlsym on the already-loaded global scope.
// Never dlopens: at constructor time we are still inside the loader's init
// phase, where a recursive load would run another object's initializers.
pw_runtime_t pw_confirm_by_symbol(pw_runtime_t hint);

// Returns the directory the agent payload was installed into, taken from
// PROCWATCH_AGENT_DIR and falling back to the compiled-in default.
const char *pw_agent_dir(void);

// setenv() that leaves an existing value alone when it already contains
// `marker`. The JDK launcher re-execs itself, so the constructor runs more
// than once and appending unconditionally would duplicate the agent.
int pw_env_append(const char *name, const char *addition, const char *marker);
int pw_env_prepend(const char *name, const char *addition, const char *marker,
                   char separator);
int pw_env_set_default(const char *name, const char *value);

void pw_apply_java(const char *agent_dir);
void pw_apply_python(const char *agent_dir);
void pw_apply_common(void);

// Validated PROCWATCH_LABEL, or NULL if missing/invalid. Required before
// OTEL bootstrap and before starting the metric thread.
const char *pw_resolve_label(void);

// Appends procwatch.label=<label> to OTEL_RESOURCE_ATTRIBUTES idempotently.
void pw_attach_label_to_otel(const char *label);

// Starts the metric push thread. Safe to call from __libc_start_main, not
// from an ELF constructor (loader lock).
void pw_metrics_thread_start(const char *runtime_hint);

// True once the constructor decided this process should be instrumented.
int pw_injection_armed(void);
void pw_set_injection_armed(int armed);
const char *pw_runtime_hint(void);
void pw_set_runtime_hint(const char *hint);

// Diagnostics go to stderr only when PROCWATCH_INJECT_DEBUG is set; an
// injector that chatters on every process start is unusable.
void pw_debug(const char *fmt, ...);
