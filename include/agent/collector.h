#pragma once

#include <sys/types.h>
#include <postgresql/libpq-fe.h>

#define PW_SERVICE_MAX 256
#define PW_CONTAINER_ID_MAX 80
#define PW_COMM_MAX 64
#define PW_LABEL_MAX 100

// Optional hostPID fallback collector. Prefer inject-thread / wrap pushes;
// enable with -A only when you still want node-wide scrape of labeled procs.

typedef struct {
    pid_t pid;
    unsigned long long starttime;
    unsigned long long proc_time;
    char label[PW_LABEL_MAX];
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

    char schema[PW_LABEL_MAX];
    unsigned long long samples_written;
} pw_collector_t;

void collector_init(pw_collector_t *c, const char *schema);
void collector_free(pw_collector_t *c);
int collector_tick(pw_collector_t *c, PGconn *conn);
