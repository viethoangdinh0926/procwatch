#pragma once

#include <sys/types.h>
#include <postgresql/libpq-fe.h>

#define PW_SERVICE_MAX 256
#define PW_CONTAINER_ID_MAX 80
#define PW_COMM_MAX 64

// A process the collector is tracking, with enough state to turn cumulative
// jiffie counters into a CPU percentage across samples.
typedef struct {
    pid_t pid;
    unsigned long long starttime;   // guards against PID reuse between ticks
    unsigned long long proc_time;
    char service[PW_SERVICE_MAX];
    char container_id[PW_CONTAINER_ID_MAX];
    char pod[PW_SERVICE_MAX];
    char comm[PW_COMM_MAX];
    const char *runtime;
    int seen;
    int identified;
} pw_proc_t;

typedef struct {
    pw_proc_t *procs;
    size_t count;
    size_t cap;

    unsigned long long prev_cpu_total;
    long clk_tck;
    long page_kb;
    long ncpu;

    // Only processes carrying PROCWATCH_SERVICE in their environment are
    // recorded when set, which keeps a hostPID DaemonSet from writing a row
    // for every process on the node.
    int require_service_env;
    unsigned long long samples_written;
} pw_collector_t;

void collector_init(pw_collector_t *c, int require_service_env);
void collector_free(pw_collector_t *c);

// Walks /proc, computes deltas against the previous tick, and writes one row
// per tracked process. Returns the number of rows written.
int collector_tick(pw_collector_t *c, PGconn *conn);
