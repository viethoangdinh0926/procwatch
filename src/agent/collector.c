// Process metric collection for agentd.
//
// Unlike the procwatch binary, which walks the tree below one known root,
// agentd does not know its targets in advance: it runs as a DaemonSet with
// hostPID and discovers every process on the node. Identity comes from two
// places in /proc that the binary never needed:
//   - /proc/<pid>/environ, for the PROCWATCH_SERVICE the injector planted,
//     which is what joins these rows to the spans from the same service.
//   - /proc/<pid>/cgroup, for the container id.
//
// This is also the only path by which Go workloads are observed at all, since
// they cannot be injected.

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../include/proc_scan.h"
#include "../../include/util.h"
#include "../../include/agent/collector.h"
#include "../../include/agent/db_otlp.h"

#define ENVIRON_READ_MAX 32768

void collector_init(pw_collector_t *c, const char *schema) {
    memset(c, 0, sizeof *c);
    c->clk_tck = sysconf(_SC_CLK_TCK);
    if (c->clk_tck <= 0) c->clk_tck = 100;
    c->page_kb = sysconf(_SC_PAGESIZE) / 1024;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    c->ncpu = (ncpu > 0) ? ncpu : 1;
    if (schema) snprintf(c->schema, sizeof c->schema, "%s", schema);

    cpu_totals_t tot;
    if (parse_cpu_totals(&tot) == 0) c->prev_cpu_total = cpu_totals_sum(&tot);
}

void collector_free(pw_collector_t *c) {
    free(c->procs);
    c->procs = NULL;
    c->count = c->cap = 0;
}

// ------------------------ /proc identity helpers ------------------------

// Pulls one variable out of the NUL-separated /proc/<pid>/environ blob.
// Requires matching uid or CAP_SYS_PTRACE, which the DaemonSet has and an
// unprivileged sidecar does not; a failure here just means the process goes
// unlabelled.
static int read_environ_var(pid_t pid, const char *key, char *out, size_t cap) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/environ", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    char buf[ENVIRON_READ_MAX];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    size_t keylen = strlen(key);
    for (ssize_t i = 0; i < n; ) {
        const char *entry = buf + i;
        size_t entry_len = strlen(entry);
        if (entry_len > keylen && entry[keylen] == '=' &&
            strncmp(entry, key, keylen) == 0) {
            snprintf(out, cap, "%s", entry + keylen + 1);
            return 0;
        }
        i += (ssize_t)entry_len + 1;
    }
    return -1;
}

// Extracts the container id from a cgroup path. Handles both cgroup v2
// ("0::/kubepods/.../docker-<id>.scope") and v1, and the several id shapes
// runtimes use: "docker-<64hex>.scope", "crio-<64hex>.scope", or a bare
// 64-hex path segment.
static int extract_container_id(const char *cgroup, char *out, size_t cap) {
    const char *best = NULL;
    size_t best_len = 0;

    const char *p = cgroup;
    while (*p) {
        while (*p == '/' || *p == ':') ++p;
        const char *seg = p;
        while (*p && *p != '/' && *p != ':') ++p;
        size_t seg_len = (size_t)(p - seg);
        if (!seg_len) continue;

        const char *id = seg;
        size_t id_len = seg_len;

        // Strip a "<runtime>-" prefix and a ".scope"/".slice" suffix.
        const char *dash = memchr(id, '-', id_len);
        if (dash) {
            size_t prefix = (size_t)(dash - id);
            if (prefix <= 8) {
                id = dash + 1;
                id_len = seg_len - prefix - 1;
            }
        }
        const char *dot = memchr(id, '.', id_len);
        if (dot) id_len = (size_t)(dot - id);

        int all_hex = id_len >= 12;
        for (size_t i = 0; i < id_len && all_hex; ++i) {
            if (!isxdigit((unsigned char)id[i])) all_hex = 0;
        }
        if (all_hex && id_len > best_len) {
            best = id;
            best_len = id_len;
        }
    }

    if (!best) return -1;
    if (best_len >= cap) best_len = cap - 1;
    memcpy(out, best, best_len);
    out[best_len] = '\0';
    return 0;
}

// Best-effort runtime label. Java and Python are known from the injector's
// own marker; anything else is classified from the executable so that Go
// workloads are at least distinguishable in the data.
static const char *classify_runtime(pid_t pid, const char *comm) {
    char path[64], target[512];
    snprintf(path, sizeof path, "/proc/%d/exe", pid);
    ssize_t n = readlink(path, target, sizeof target - 1);
    if (n > 0) {
        target[n] = '\0';
        const char *slash = strrchr(target, '/');
        const char *base = slash ? slash + 1 : target;
        if (strcmp(base, "java") == 0) return "java";
        if (strncmp(base, "python", 6) == 0) return "python";
        if (strcmp(base, "node") == 0) return "nodejs";
    }
    if (comm) {
        if (strcmp(comm, "java") == 0) return "java";
        if (strncmp(comm, "python", 6) == 0) return "python";
    }
    return "other";
}

// ------------------------ Tracking table ------------------------

static pw_proc_t *find_or_add(pw_collector_t *c, pid_t pid,
                              unsigned long long starttime, int *is_new) {
    for (size_t i = 0; i < c->count; ++i) {
        if (c->procs[i].pid != pid) continue;
        // A recycled PID looks identical apart from its start time; treating
        // it as the same process would emit a nonsense CPU delta.
        if (c->procs[i].starttime != starttime) {
            memset(&c->procs[i], 0, sizeof c->procs[i]);
            c->procs[i].pid = pid;
            c->procs[i].starttime = starttime;
            *is_new = 1;
            return &c->procs[i];
        }
        *is_new = 0;
        return &c->procs[i];
    }

    if (c->count == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 256;
        pw_proc_t *tmp = realloc(c->procs, cap * sizeof *tmp);
        if (!tmp) return NULL;
        c->procs = tmp;
        c->cap = cap;
    }
    pw_proc_t *p = &c->procs[c->count++];
    memset(p, 0, sizeof *p);
    p->pid = pid;
    p->starttime = starttime;
    *is_new = 1;
    return p;
}

static void drop_unseen(pw_collector_t *c) {
    size_t out = 0;
    for (size_t i = 0; i < c->count; ++i) {
        if (c->procs[i].seen) c->procs[out++] = c->procs[i];
    }
    c->count = out;
}

int collector_tick(pw_collector_t *c, PGconn *conn) {
    cpu_totals_t tot;
    if (parse_cpu_totals(&tot) != 0) return 0;
    unsigned long long now_total = cpu_totals_sum(&tot);
    unsigned long long delta_total = (now_total > c->prev_cpu_total)
                                   ? now_total - c->prev_cpu_total : 0;

    DIR *d = opendir("/proc");
    if (!d) return 0;

    for (size_t i = 0; i < c->count; ++i) c->procs[i].seen = 0;

    int written = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char *endp = NULL;
        long pidl = strtol(de->d_name, &endp, 10);
        if (!endp || *endp != '\0' || pidl <= 0) continue;
        pid_t pid = (pid_t)pidl;

        char comm[PW_COMM_MAX];
        proc_stat_t st;
        if (read_proc_stat(pid, &st, comm, sizeof comm) != 0) continue;

        int is_new = 0;
        pw_proc_t *p = find_or_add(c, pid, st.starttime, &is_new);
        if (!p) continue;
        p->seen = 1;

        // Identity is resolved once per process: environ and cgroup do not
        // change over a process's lifetime, and re-reading them for every
        // process on every tick is the most expensive thing this loop could do.
        if (!p->identified) {
            p->identified = 1;
            snprintf(p->comm, sizeof p->comm, "%s", comm);
            p->runtime = classify_runtime(pid, comm);

            if (read_environ_var(pid, "PROCWATCH_LABEL", p->label,
                                 sizeof p->label) != 0) {
                p->label[0] = '\0';
            }

            if (read_environ_var(pid, "PROCWATCH_SERVICE", p->service,
                                 sizeof p->service) != 0) {
                if (read_environ_var(pid, "OTEL_SERVICE_NAME", p->service,
                                     sizeof p->service) != 0) {
                    p->service[0] = '\0';
                }
            }
            read_environ_var(pid, "PROCWATCH_POD", p->pod, sizeof p->pod);

            char cgroup[512];
            if (read_cgroup(pid, cgroup, sizeof cgroup) == 0) {
                if (extract_container_id(cgroup, p->container_id,
                                         sizeof p->container_id) != 0) {
                    p->container_id[0] = '\0';
                }
            }
        }

        unsigned long long proc_time = st.utime + st.stime;
        unsigned long long delta_proc = (proc_time >= p->proc_time)
                                      ? proc_time - p->proc_time : 0;
        unsigned long long prev_proc_time = p->proc_time;
        p->proc_time = proc_time;

        // No baseline yet, so any percentage would be the process's lifetime
        // average rather than the interval's.
        if (is_new && prev_proc_time == 0) continue;

        // Host scrape only records processes that carry a table label.
        if (!p->label[0] || !validate_identifier(p->label)) continue;

        double cpu_pct = 0.0;
        if (delta_total > 0) {
            cpu_pct = (100.0 * (double)delta_proc / (double)delta_total) * (double)c->ncpu;
        }

        proc_status_t status;
        long rss_kb = st.rss * c->page_kb;
        long threads = st.num_threads;
        if (read_proc_status(pid, &status) == 0) {
            if (status.vmrss_kb > 0) rss_kb = status.vmrss_kb;
            if (status.threads > 0) threads = status.threads;
        }

        if (db_ensure_label(conn, c->schema, p->label) != 0) continue;

        const char *service = p->service[0] ? p->service : p->comm;
        if (db_insert_metric_labeled(conn, c->schema, p->label, service,
                                     p->container_id, p->pod, (int)pid,
                                     p->comm, p->runtime ? p->runtime : "other",
                                     cpu_pct, rss_kb, threads) == 0) {
            ++written;
            ++c->samples_written;
        }
    }

    closedir(d);
    drop_unseen(c);
    c->prev_cpu_total = now_total;
    return written;
}
