// Keep PROCWATCH_LABEL / LD_PRELOAD / related vars across exec when a parent
// rebuilds envp from scratch (common for language runtimes and supervisors).

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../include/inject.h"

extern void *dlsym(void *handle, const char *symbol) __attribute__((weak));

#ifndef RTLD_NEXT
#define RTLD_NEXT ((void *)-1l)
#endif

static const char *KEEP_KEYS[] = {
    "PROCWATCH_LABEL",
    "PROCWATCH_SERVICE",
    "PROCWATCH_ENDPOINT",
    "PROCWATCH_AGENT_DIR",
    "PROCWATCH_POD",
    "PROCWATCH_METRIC_INTERVAL",
    "PROCWATCH_INJECT_DEBUG",
    "LD_PRELOAD",
    "OTEL_SERVICE_NAME",
    "OTEL_EXPORTER_OTLP_ENDPOINT",
    "OTEL_EXPORTER_OTLP_PROTOCOL",
    "OTEL_RESOURCE_ATTRIBUTES",
    "OTEL_TRACES_EXPORTER",
    "OTEL_METRICS_EXPORTER",
    "OTEL_LOGS_EXPORTER",
    "JAVA_TOOL_OPTIONS",
    "PYTHONPATH",
    NULL
};

static int env_has_key(char *const envp[], const char *key) {
    if (!envp) return 0;
    size_t klen = strlen(key);
    for (char *const *e = envp; *e; ++e) {
        if (strncmp(*e, key, klen) == 0 && (*e)[klen] == '=') return 1;
    }
    return 0;
}

// Returns a newly allocated envp that merges missing KEEP_KEYS from the
// current environ into envp. Caller frees the array (and the strdup'd entries
// we added) only on the failure path before exec; on success exec replaces us.
static char **merge_env(char *const envp[]) {
    // Count existing.
    size_t n = 0;
    if (envp) for (char *const *e = envp; *e; ++e) ++n;

    size_t add = 0;
    for (int i = 0; KEEP_KEYS[i]; ++i) {
        const char *val = getenv(KEEP_KEYS[i]);
        if (!val || !*val) continue;
        if (env_has_key(envp, KEEP_KEYS[i])) continue;
        ++add;
    }
    if (add == 0) return NULL; // nothing to merge; use original envp

    char **out = calloc(n + add + 1, sizeof *out);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) out[j++] = envp[i];
    for (int i = 0; KEEP_KEYS[i]; ++i) {
        const char *key = KEEP_KEYS[i];
        const char *val = getenv(key);
        if (!val || !*val) continue;
        if (env_has_key(envp, key)) continue;
        size_t len = strlen(key) + 1 + strlen(val) + 1;
        char *entry = malloc(len);
        if (!entry) {
            // Best-effort: free what we allocated and fall back to original.
            for (size_t k = n; k < j; ++k) free(out[k]);
            free(out);
            return NULL;
        }
        snprintf(entry, len, "%s=%s", key, val);
        out[j++] = entry;
    }
    out[j] = NULL;
    return out;
}

typedef int (*execve_t)(const char *, char *const[], char *const[]);
typedef int (*execvpe_t)(const char *, char *const[], char *const[]);
typedef int (*execveat_t)(int, const char *, char *const[], char *const[], int);

__attribute__((visibility("default")))
int execve(const char *pathname, char *const argv[], char *const envp[]) {
    execve_t real = dlsym ? (execve_t)dlsym(RTLD_NEXT, "execve") : NULL;
    if (!real) {
        // No real symbol; cannot proceed.
        return -1;
    }
    char **merged = merge_env(envp);
    int rc = real(pathname, argv, merged ? merged : envp);
    // Only reached on failure.
    if (merged) {
        // Free only the entries we allocated (after the original n). Hard to
        // track — free the whole array of pointers we strdup'd by scanning
        // KEEP_KEYS we may have added. Simpler: leak on the rare failure path.
        free(merged);
    }
    return rc;
}

__attribute__((visibility("default")))
int execvpe(const char *file, char *const argv[], char *const envp[]) {
    execvpe_t real = dlsym ? (execvpe_t)dlsym(RTLD_NEXT, "execvpe") : NULL;
    if (!real) {
        // Fall back through execve with PATH search omitted; return error.
        errno = ENOSYS;
        return -1;
    }
    char **merged = merge_env(envp);
    int rc = real(file, argv, merged ? merged : envp);
    if (merged) free(merged);
    return rc;
}

__attribute__((visibility("default")))
int execveat(int dirfd, const char *pathname, char *const argv[],
             char *const envp[], int flags) {
    execveat_t real = dlsym ? (execveat_t)dlsym(RTLD_NEXT, "execveat") : NULL;
    if (!real) {
        errno = ENOSYS;
        return -1;
    }
    char **merged = merge_env(envp);
    int rc = real(dirfd, pathname, argv, merged ? merged : envp, flags);
    if (merged) free(merged);
    return rc;
}
