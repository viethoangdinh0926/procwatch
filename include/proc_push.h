#pragma once

// Shared process-metric sampling and HTTP push used by both the inject
// metric thread and procwatch-wrap. libc-only; safe to link into the
// LD_PRELOAD library.

#include <sys/types.h>

#define PW_LABEL_MAX 100
#define PW_PUSH_SERVICE_MAX 256
#define PW_PUSH_COMM_MAX 64
#define PW_PUSH_RUNTIME_MAX 32

typedef struct {
    char label[PW_LABEL_MAX];
    char service[PW_PUSH_SERVICE_MAX];
    char comm[PW_PUSH_COMM_MAX];
    char runtime[PW_PUSH_RUNTIME_MAX];
    char pod[PW_PUSH_SERVICE_MAX];
    char container_id[80];
    int pid;
    double cpu_pct;
    long rss_kb;
    long threads;
} pw_proc_sample_t;

// Cumulative counters retained between samples for CPU%.
typedef struct {
    unsigned long long proc_time;
    unsigned long long cpu_total;
    int have_baseline;
} pw_proc_baseline_t;

int pw_label_valid(const char *label);

// Reads /proc/<pid>/{stat,status} and updates baseline. Returns 0 on success.
// On the first call (no baseline) cpu_pct is 0 and have_baseline becomes 1
// without implying a publishable sample — caller should skip the first tick.
int pw_sample_pid(pid_t pid, pw_proc_baseline_t *base, pw_proc_sample_t *out);

// Fills identity fields from the current process environment / exe.
void pw_sample_fill_identity(pw_proc_sample_t *out, const char *runtime_hint);

// POSTs JSON to {endpoint}/v1/procmetrics. endpoint looks like
// http://host:port or http://host:port/. Returns 0 on HTTP 2xx.
int pw_push_sample(const char *endpoint, const pw_proc_sample_t *s);
