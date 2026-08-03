// procwatch-wrap — parent-process metric sampler for static binaries
// (notably Go with CGO_ENABLED=0) that cannot load LD_PRELOAD.
//
// Usage: procwatch-wrap [--] <command> [args...]
// Requires PROCWATCH_LABEL in the environment.

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "proc_push.h"

#ifndef PW_DEFAULT_ENDPOINT
#define PW_DEFAULT_ENDPOINT "http://127.0.0.1:4318"
#endif

static volatile sig_atomic_t g_child_dead = 0;

static void on_chld(int sig) {
    (void)sig;
    g_child_dead = 1;
}

static int guess_runtime(const char *path) {
    (void)path;
    return 0; // always "other" for wrap; caller can set via env later
}

int main(int argc, char **argv) {
    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "--") == 0) ++argi;
    if (argi >= argc) {
        fprintf(stderr, "Usage: %s [--] <command> [args...]\n", argv[0]);
        return 2;
    }

    const char *label = getenv("PROCWATCH_LABEL");
    if (!pw_label_valid(label)) {
        fprintf(stderr, "procwatch-wrap: PROCWATCH_LABEL missing or invalid\n");
        return 2;
    }

    const char *endpoint = getenv("PROCWATCH_ENDPOINT");
    if (!endpoint || !*endpoint) endpoint = PW_DEFAULT_ENDPOINT;

    int interval = 10;
    const char *iv = getenv("PROCWATCH_METRIC_INTERVAL");
    if (iv && *iv) {
        int n = atoi(iv);
        if (n > 0) interval = n;
    }

    signal(SIGCHLD, on_chld);

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        execvp(argv[argi], &argv[argi]);
        perror(argv[argi]);
        _exit(127);
    }

    pw_proc_baseline_t base;
    memset(&base, 0, sizeof base);

    // Baseline tick.
    {
        pw_proc_sample_t s;
        memset(&s, 0, sizeof s);
        if (pw_sample_pid(child, &base, &s) == 0)
            pw_sample_fill_identity(&s, "other");
    }

    int status = 0;
    while (!g_child_dead) {
        // Sleep in 1s slices so we notice child exit promptly.
        for (int i = 0; i < interval && !g_child_dead; ++i) sleep(1);
        if (g_child_dead) break;

        pw_proc_sample_t s;
        memset(&s, 0, sizeof s);
        if (pw_sample_pid(child, &base, &s) != 0) continue;
        pw_sample_fill_identity(&s, "other");
        (void)guess_runtime;
        pw_push_sample(endpoint, &s);
    }

    // Reap.
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
