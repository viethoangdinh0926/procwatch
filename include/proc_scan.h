#pragma once

#include <sys/types.h>

// /proc/stat totals
typedef struct {
    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
    unsigned long long steal;
} cpu_totals_t;

// /proc/<pid>/stat subset
typedef struct {
    unsigned long long utime;
    unsigned long long stime;
    unsigned long long minflt;
    unsigned long long majflt;
    long priority;
    long nice;
    long num_threads;
    unsigned long long starttime;
    unsigned long vsize;
    long rss;
    unsigned long ppid;
} proc_stat_t;

// /proc/<pid>/status subset
typedef struct {
    long vmrss_kb;
    long vmsize_kb;
    long vmswap_kb;
    long threads;
    unsigned long long vctx;
    unsigned long long nvctx;
} proc_status_t;

typedef struct {
    long rss_kb;
    long pss_kb;
    long swap_kb;
    int have_rollup;
} proc_smaps_rollup_t;

typedef struct {
    unsigned long long rchar;
    unsigned long long wchar;
    unsigned long long syscr;
    unsigned long long syscw;
    unsigned long long read_bytes;
    unsigned long long write_bytes;
    unsigned long long cancelled_write_bytes;
} proc_io_t;

// Snapshot of a process
typedef struct {
    pid_t pid;
    pid_t ppid;
    proc_stat_t stat;
    proc_status_t status;
} proc_snapshot_t;

int parse_cpu_totals(cpu_totals_t *t);
unsigned long long cpu_totals_sum(const cpu_totals_t *t);
int read_proc_stat(pid_t pid, proc_stat_t *st, char *comm_buf, size_t comm_sz);
int read_proc_status(pid_t pid, proc_status_t *ps);
int read_proc_smaps_rollup(pid_t pid, proc_smaps_rollup_t *sr);
int read_proc_io(pid_t pid, proc_io_t *pio);
int count_fds(pid_t pid, int *fd_count, int *socket_count);
int read_first_line(const char *path, char *buf, size_t bufsz);
int read_cgroup(pid_t pid, char *buf, size_t bufsz);
int read_limits_excerpt(pid_t pid, long *max_fds_soft, long *max_fds_hard, long *stack_soft, long *stack_hard);
int read_children_pids(pid_t pid, pid_t **out, size_t *out_count, size_t *out_cap);
int pid_seen(pid_t pid, pid_t *queue, size_t q_count, proc_snapshot_t *snaps, size_t sn_count);
