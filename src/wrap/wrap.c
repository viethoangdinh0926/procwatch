// procwatch-wrap — parent-process metric sampler for static binaries
// (notably Go with CGO_ENABLED=0) that cannot load LD_PRELOAD.
//
// Usage: procwatch-wrap [--] <command> [args...]
// Requires PROCWATCH_LABEL in the environment.
//
// Each scan round BFS-walks the process tree rooted at the child app and
// POSTs one /v1/procmetrics sample per live pid under that root.

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "proc_push.h"
#include "proc_scan.h"

#ifndef PW_DEFAULT_ENDPOINT
#define PW_DEFAULT_ENDPOINT "http://127.0.0.1:4318"
#endif

static volatile sig_atomic_t g_child_dead = 0;

static void on_chld(int sig) {
    (void)sig;
    g_child_dead = 1;
}

typedef struct {
    pid_t pid;
    unsigned long long proc_time;
    int seen;
} prev_sample_t;

typedef struct {
    pid_t pid;
    proc_stat_t stat;
    proc_status_t status;
    char comm[PW_PUSH_COMM_MAX];
} tree_node_t;

static int snapshot_tree(pid_t root, tree_node_t **nodes_out, size_t *count_out) {
    tree_node_t *nodes = NULL;
    size_t n_count = 0, n_cap = 0;
    pid_t *queue = NULL;
    size_t q_head = 0, q_count = 0, q_cap = 0;
    proc_snapshot_t *seen_snaps = NULL; // only for pid_seen helper
    size_t sn_count = 0, sn_cap = 0;

    q_cap = 16;
    queue = realloc(queue, q_cap * sizeof(*queue));
    if (!queue) return -1;
    queue[q_count++] = root;

    while (q_head < q_count) {
        pid_t cur = queue[q_head++];

        tree_node_t node;
        memset(&node, 0, sizeof node);
        if (read_proc_stat(cur, &node.stat, node.comm, sizeof node.comm) != 0)
            continue;
        node.pid = cur;
        read_proc_status(cur, &node.status);

        if (n_count == n_cap) {
            n_cap = n_cap ? n_cap * 2 : 128;
            nodes = realloc(nodes, n_cap * sizeof(*nodes));
            if (!nodes) { free(queue); free(seen_snaps); return -1; }
        }
        nodes[n_count++] = node;

        if (sn_count == sn_cap) {
            sn_cap = sn_cap ? sn_cap * 2 : 128;
            seen_snaps = realloc(seen_snaps, sn_cap * sizeof(*seen_snaps));
            if (!seen_snaps) { free(queue); free(nodes); return -1; }
        }
        seen_snaps[sn_count].pid = cur;
        seen_snaps[sn_count].ppid = (pid_t)node.stat.ppid;
        ++sn_count;

        pid_t *kids = NULL;
        size_t kids_count = 0, kids_cap = 0;
        if (read_children_pids(cur, &kids, &kids_count, &kids_cap) == 0) {
            for (size_t k = 0; k < kids_count; ++k) {
                if (!pid_seen(kids[k], queue, q_count, seen_snaps, sn_count)) {
                    if (q_count == q_cap) {
                        q_cap = q_cap ? q_cap * 2 : 16;
                        queue = realloc(queue, q_cap * sizeof(*queue));
                        if (!queue) {
                            free(kids); free(nodes); free(seen_snaps);
                            return -1;
                        }
                    }
                    queue[q_count++] = kids[k];
                }
            }
        }
        free(kids);
    }
    free(queue);
    free(seen_snaps);
    *nodes_out = nodes;
    *count_out = n_count;
    return 0;
}

static void push_tree(const char *endpoint, const char *created_stamp,
                      pid_t root, prev_sample_t **prev, size_t *prev_count,
                      size_t *prev_cap, cpu_totals_t *tot_prev, long ncpu,
                      long page_kb, int skip_push) {
    cpu_totals_t tot_now;
    if (parse_cpu_totals(&tot_now) != 0) return;
    unsigned long long delta_tot =
        cpu_totals_sum(&tot_now) - cpu_totals_sum(tot_prev);

    tree_node_t *nodes = NULL;
    size_t n_count = 0;
    if (snapshot_tree(root, &nodes, &n_count) != 0) return;

    for (size_t i = 0; i < *prev_count; ++i) (*prev)[i].seen = 0;

    for (size_t i = 0; i < n_count; ++i) {
        unsigned long long proc_time = nodes[i].stat.utime + nodes[i].stat.stime;
        unsigned long long delta_proc = 0;
        int found = 0;
        for (size_t p = 0; p < *prev_count; ++p) {
            if ((*prev)[p].pid == nodes[i].pid) {
                (*prev)[p].seen = 1;
                if (proc_time >= (*prev)[p].proc_time)
                    delta_proc = proc_time - (*prev)[p].proc_time;
                (*prev)[p].proc_time = proc_time;
                found = 1;
                break;
            }
        }
        if (!found) {
            if (*prev_count == *prev_cap) {
                *prev_cap = *prev_cap ? *prev_cap * 2 : 128;
                *prev = realloc(*prev, *prev_cap * sizeof(**prev));
                if (!*prev) { free(nodes); return; }
            }
            (*prev)[(*prev_count)++] = (prev_sample_t){
                .pid = nodes[i].pid, .proc_time = proc_time, .seen = 1
            };
            delta_proc = 0;
        }

        if (skip_push) continue;

        double cpu_pct = 0.0;
        if (delta_tot > 0)
            cpu_pct = (100.0 * (double)delta_proc / (double)delta_tot) * (double)ncpu;

        long rss_kb = nodes[i].status.vmrss_kb > 0
                          ? nodes[i].status.vmrss_kb
                          : nodes[i].stat.rss * page_kb;
        long threads = nodes[i].status.threads > 0
                           ? nodes[i].status.threads
                           : nodes[i].stat.num_threads;

        pw_proc_sample_t s;
        memset(&s, 0, sizeof s);
        s.pid = (int)nodes[i].pid;
        s.cpu_pct = cpu_pct;
        s.rss_kb = rss_kb;
        s.threads = threads;
        snprintf(s.comm, sizeof s.comm, "%s", nodes[i].comm);
        pw_sample_fill_identity(&s, "other");
        pw_sample_set_pid_key(&s, s.pid, created_stamp);
        if (pw_label_valid(s.label))
            pw_push_sample(endpoint, &s);
    }

    free(nodes);
    *tot_prev = tot_now;
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

    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (page_kb <= 0) page_kb = 4;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu <= 0) ncpu = 1;

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

    char created_stamp[32];
    pw_format_created_stamp(created_stamp, sizeof created_stamp, time(NULL));

    prev_sample_t *prev = NULL;
    size_t prev_count = 0, prev_cap = 0;
    cpu_totals_t tot_prev;
    memset(&tot_prev, 0, sizeof tot_prev);
    parse_cpu_totals(&tot_prev);

    // Baseline round: establish per-pid CPU counters, do not push.
    push_tree(endpoint, created_stamp, child, &prev, &prev_count, &prev_cap,
              &tot_prev, ncpu, page_kb, /*skip_push=*/1);

    int status = 0;
    while (!g_child_dead) {
        for (int i = 0; i < interval && !g_child_dead; ++i) sleep(1);
        if (g_child_dead) break;

        push_tree(endpoint, created_stamp, child, &prev, &prev_count, &prev_cap,
                  &tot_prev, ncpu, page_kb, /*skip_push=*/0);
    }

    free(prev);

    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
