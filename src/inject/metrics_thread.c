// Metric push thread for libprocwatch_inject.so.
//
// Started from the interposed __libc_start_main, not from the ELF
// constructor, because pthread_create under the loader lock is unsafe.

#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../include/inject.h"
#include "../../include/proc_push.h"

#ifndef PW_DEFAULT_ENDPOINT
#define PW_DEFAULT_ENDPOINT "http://127.0.0.1:4318"
#endif

static void *metrics_loop(void *arg) {
    const char *runtime = (const char *)arg;
    const char *endpoint = getenv("PROCWATCH_ENDPOINT");
    if (!endpoint || !*endpoint) endpoint = PW_DEFAULT_ENDPOINT;

    int interval = 10;
    const char *iv = getenv("PROCWATCH_METRIC_INTERVAL");
    if (iv && *iv) {
        int n = atoi(iv);
        if (n > 0) interval = n;
    }

    // Capture sampler start once so every sample shares a stable series key
    // "<pid>_<YYYYMMDDHHMMSS>" for the life of this process/thread.
    char created_stamp[32];
    pw_format_created_stamp(created_stamp, sizeof created_stamp, time(NULL));

    pw_proc_baseline_t base;
    memset(&base, 0, sizeof base);

    // First sample establishes the CPU baseline only.
    {
        pw_proc_sample_t s;
        memset(&s, 0, sizeof s);
        if (pw_sample_pid(getpid(), &base, &s) == 0)
            pw_sample_fill_identity(&s, runtime);
    }

    for (;;) {
        sleep((unsigned)interval);
        pw_proc_sample_t s;
        memset(&s, 0, sizeof s);
        if (pw_sample_pid(getpid(), &base, &s) != 0) continue;
        pw_sample_fill_identity(&s, runtime);
        pw_sample_set_pid_key(&s, s.pid, created_stamp);
        if (!pw_label_valid(s.label)) continue;
        if (pw_push_sample(endpoint, &s) != 0)
            pw_debug("metric push failed");
    }
    return NULL;
}

void pw_metrics_thread_start(const char *runtime_hint) {
    static int started = 0;
    if (started) return;
    if (!pw_resolve_label()) {
        pw_debug("no valid PROCWATCH_LABEL, metric thread not started");
        return;
    }

    const char *hint = runtime_hint ? runtime_hint : "other";
    // Leak a copy so the thread keeps a stable pointer.
    char *arg = strdup(hint);
    if (!arg) arg = (char *)"other";

    pthread_t thr;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thr, &attr, metrics_loop, arg) == 0) {
        started = 1;
        pw_debug("metric thread started (%s)", hint);
    }
    pthread_attr_destroy(&attr);
}

// Interpose __libc_start_main so the metric thread starts before app main,
// after constructors (and the loader lock) have finished.
typedef int (*libc_start_main_t)(int (*main)(int, char **, char **), int argc,
                                 char **ubp_av, void (*init)(void),
                                 void (*fini)(void), void (*rtld_fini)(void),
                                 void *stack_end);

extern void *__libc_dlsym(void *handle, const char *name) __attribute__((weak));
extern void *dlsym(void *handle, const char *symbol) __attribute__((weak));

#ifndef RTLD_NEXT
#define RTLD_NEXT ((void *)-1l)
#endif

__attribute__((visibility("default")))
int __libc_start_main(int (*main)(int, char **, char **), int argc,
                      char **ubp_av, void (*init)(void), void (*fini)(void),
                      void (*rtld_fini)(void), void *stack_end) {
    libc_start_main_t real = NULL;
    if (dlsym) real = (libc_start_main_t)dlsym(RTLD_NEXT, "__libc_start_main");

    if (pw_injection_armed())
        pw_metrics_thread_start(pw_runtime_hint());

    if (!real) {
        // Without the real start_main we cannot continue; aborting here would
        // kill the process, so spin rather than return into nowhere.
        for (;;) pause();
    }
    return real(main, argc, ubp_av, init, fini, rtld_fini, stack_end);
}
