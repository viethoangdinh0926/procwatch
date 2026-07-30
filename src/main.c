// procwatch.c - Track resource usage of a Linux process via /proc and write samples to TimescaleDB
// Build: gcc -O2 -Wall -Wextra -lpq -o procwatch procwatch.c

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <limits.h>
#include <libpq-fe.h>

#include "../include/util.h"
#include "../include/proc_scan.h"
#include "../include/db.h"

// ------------------------ Globals ------------------------
static long g_clk_tck = 0;
static long g_page_kb = 0;
static long g_ncpu = 1;

typedef struct {
    pid_t pid;
    unsigned long long proc_time;
    int seen;
} prev_sample_t;

// Default DB connection string (can be overridden with -d)
static const char *DEFAULT_CONNINFO =
    "postgresql://procwatcher:procwatcherpw@10.244.234.195:5433/procwatcherdb";

static const char *DEFAULT_SCHEMA = "procwatch";

// ------------------------ CLI / usage ------------------------
static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s (-p <pid> | -c <command>) -l <label> [-i interval_sec] [-n samples] [-d conn_str] [-s schema]\n"
        "  -l <label>  REQUIRED label (<100 chars; [A-Za-z0-9_]+). Table name is <label>.\n"
        "  -p <pid>     Attach to existing process tree rooted at pid.\n"
        "  -c <command> Execute command via /bin/sh -c and watch its process tree (mutually exclusive with -p).\n",
        argv0);
}

// ------------------------ Main ------------------------
int main(int argc, char **argv) {
    pid_t pid = -1;
    pid_t child_pid = -1;
    int interval = 2;
    int samples = -1;
    const char *conninfo = DEFAULT_CONNINFO;
    const char *schema   = DEFAULT_SCHEMA;
    const char *label    = NULL;
    const char *command  = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:i:n:d:s:l:c:h")) != -1) {
        switch (opt) {
            case 'p': pid = (pid_t)atoi(optarg); break;
            case 'i': interval = atoi(optarg); break;
            case 'n': samples  = atoi(optarg); break;
            case 'd': conninfo = optarg; break;
            case 's': schema   = optarg; break;
            case 'l': label    = optarg; break;
            case 'c': command  = optarg; break;
            case 'h': default: usage(argv[0]); return (opt=='h')?0:1;
        }
    }

    if ((pid <= 0 && !command) || (pid > 0 && command)) {
        fprintf(stderr, "Error: specify exactly one of -p <pid> or -c <command>.\n");
        usage(argv[0]);
        return 1;
    }
    if (!validate_identifier(label)) {
        fprintf(stderr, "Error: -l <label> is required and must match ^[A-Za-z0-9_]+$ and be <100 chars.\n");
        return 1;
    }

    // Launch command if requested
    if (command) {
        pid_t child = fork();
        if (child < 0) die("fork failed: %s", strerror(errno));
        if (child == 0) {
            // Replace shell with the target command to avoid tracking the sh wrapper
            char *exec_cmd = NULL;
            const char prefix[] = "exec ";
            size_t len = strlen(prefix) + strlen(command) + 1;
            exec_cmd = malloc(len);
            if (!exec_cmd) _exit(127);
            snprintf(exec_cmd, len, "%s%s", prefix, command);
            execl("/bin/sh", "sh", "-c", exec_cmd, (char*)NULL);
            _exit(127);
        }
        child_pid = child;
        pid = child;
    }

    g_clk_tck = sysconf(_SC_CLK_TCK);
    g_page_kb = sysconf(_SC_PAGESIZE) / 1024;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    g_ncpu = (ncpu > 0) ? ncpu : 1;

    // Validate process existence
    char procpath[64];
    snprintf(procpath, sizeof procpath, "/proc/%d", pid);
    struct stat st;
    if (stat(procpath, &st) != 0) die("Cannot access %s: %s", procpath, strerror(errno));

    // Connect to DB and ensure pg_cron + target table and job
    PGconn *db = db_connect_or_die(conninfo);
    db_ensure_table(db, schema, label);
    db_ensure_drop_inactive_job(db);

    printf("procwatch: tracking root pid=%d (label=%s, schema=%s)%s\n",
           pid, label, schema, command ? " (spawned command)" : "");

    // Previous samples for deltas
    cpu_totals_t tot_prev = (cpu_totals_t){0}, tot_now = (cpu_totals_t){0};
    if (parse_cpu_totals(&tot_prev) != 0) die("Failed reading /proc/stat");

    prev_sample_t *prev = NULL; size_t prev_count = 0, prev_cap = 0;

    char stmt_name[64];
    snprintf(stmt_name, sizeof stmt_name, "ins_%d", (int)pid);
    db_prepare_insert(db, schema, label, stmt_name);

    int iter = 0;
    while (samples < 0 || iter < samples) {
        sleep(interval);

        // Process still exists?
        if (stat(procpath, &st) != 0) {
            fprintf(stderr, "Root process %d exited (or access lost).\n", pid);
            break;
        }

        if (parse_cpu_totals(&tot_now) != 0) break;
        unsigned long long delta_tot  = cpu_totals_sum(&tot_now) - cpu_totals_sum(&tot_prev);

        // Snapshot processes via BFS from root using /proc/<pid>/task/<pid>/children
        proc_snapshot_t *snaps = NULL; size_t sn_count = 0, sn_cap = 0;
        pid_t *queue = NULL; size_t q_head = 0, q_count = 0, q_cap = 0;

        // enqueue root
        if (q_count == q_cap) { q_cap = 16; queue = realloc(queue, q_cap * sizeof(*queue)); if (!queue) die("OOM"); }
        queue[q_count++] = pid;

        while (q_head < q_count) {
            pid_t cur = queue[q_head++];

            // read stat/status
            proc_stat_t stbuf;
            if (read_proc_stat(cur, &stbuf, NULL, 0) != 0) continue;

            proc_snapshot_t snap; memset(&snap, 0, sizeof snap);
            snap.pid = cur;
            snap.ppid = (pid_t)stbuf.ppid;
            snap.stat = stbuf;
            read_proc_status(cur, &snap.status);

            if (sn_count == sn_cap) { sn_cap = sn_cap ? sn_cap*2 : 128; snaps = realloc(snaps, sn_cap * sizeof(*snaps)); if(!snaps) die("OOM"); }
            snaps[sn_count++] = snap;

            // enqueue children
            pid_t *kids = NULL; size_t kids_count = 0, kids_cap = 0;
            if (read_children_pids(cur, &kids, &kids_count, &kids_cap) == 0) {
                for (size_t k = 0; k < kids_count; ++k) {
                    if (!pid_seen(kids[k], queue, q_count, snaps, sn_count)) {
                        if (q_count == q_cap) { q_cap = q_cap ? q_cap*2 : 16; queue = realloc(queue, q_cap * sizeof(*queue)); if(!queue) die("OOM"); }
                        queue[q_count++] = kids[k];
                    }
                }
            }
            free(kids);
        }

        free(queue);

        // Reset seen flags for prev map
        for (size_t i = 0; i < prev_count; ++i) prev[i].seen = 0;

        for (size_t i = 0; i < sn_count; ++i) {
            unsigned long long proc_time = snaps[i].stat.utime + snaps[i].stat.stime;
            unsigned long long delta_proc = 0;
            int found = 0;
            for (size_t p = 0; p < prev_count; ++p) {
                if (prev[p].pid == snaps[i].pid) {
                    prev[p].seen = 1;
                    if (proc_time >= prev[p].proc_time)
                        delta_proc = proc_time - prev[p].proc_time;
                    prev[p].proc_time = proc_time;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (prev_count == prev_cap) { prev_cap = prev_cap ? prev_cap*2 : 128; prev = realloc(prev, prev_cap * sizeof(*prev)); if(!prev) die("OOM"); }
                prev[prev_count++] = (prev_sample_t){ .pid = snaps[i].pid, .proc_time = proc_time, .seen = 1 };
                delta_proc = 0;
            }

            double cpu_pct = 0.0;
            if (delta_tot > 0) cpu_pct = (100.0 * (double)delta_proc / (double)delta_tot) * (double)g_ncpu;
            long rss_kb = snaps[i].status.vmrss_kb > 0 ? snaps[i].status.vmrss_kb : snaps[i].stat.rss * g_page_kb;

            db_insert_sample(db, stmt_name, snaps[i].pid, cpu_pct, rss_kb);
        }

        free(snaps);
        tot_prev = tot_now;
        ++iter;
    }

    if (db) PQfinish(db);
    if (child_pid > 0) {
        int status;
        waitpid(child_pid, &status, WNOHANG);
    }
    return 0;
}