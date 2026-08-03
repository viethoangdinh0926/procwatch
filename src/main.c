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
#include "../include/db_buffer.h"

// ------------------------ Globals ------------------------
static long g_clk_tck = 0;
static long g_page_kb = 0;
static long g_ncpu = 1;

typedef struct {
    pid_t pid;
    unsigned long long proc_time;
    int seen;
} prev_sample_t;

typedef struct {
    pw_db_buf_t *buf;
    const char *schema;
    const char *label;
    char stmt_name[64];
    int prepared;
} pw_flush_ctx_t;

// Default DB connection string (can be overridden with -d)
static const char *DEFAULT_CONNINFO =
    "postgresql://procwatcher:procwatcherpw@10.244.234.195:5433/procwatcherdb";

static const char *DEFAULT_SCHEMA = "procwatch";

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s (-p <pid> | -c <command>) -l <label> [-i interval_sec] [-n samples]\n"
        "          [-d conn_str] [-s schema] [-R retention_hours] [-T inactive_hours]\n"
        "  -l <label>   REQUIRED label (<100 chars; [A-Za-z0-9_]+). Table name is <label>.\n"
        "  -p <pid>     Attach to existing process tree rooted at pid.\n"
        "  -c <command> Execute command via /bin/sh -c and watch its process tree (mutually exclusive with -p).\n"
        "  -R <hours>   Timescale chunk retention (default %d, env PROCWATCH_RETENTION_HOURS)\n"
        "  -T <hours>   Drop tables idle longer than this (default %d, env PROCWATCH_INACTIVE_HOURS)\n"
        "\n"
        "If the database is unreachable, samples are spilled under\n"
        "PROCWATCH_SPILL_DIR (default /var/tmp/procwatch) and flushed on reconnect.\n",
        argv0, PW_DEFAULT_RETENTION_HOURS, PW_DEFAULT_INACTIVE_HOURS);
}

static int ensure_prepared(pw_flush_ctx_t *ctx) {
    if (!ctx->buf->online || !ctx->buf->conn) return -1;
    if (ctx->prepared) return 0;
    db_ensure_table(ctx->buf->conn, ctx->schema, ctx->label);
    db_ensure_drop_inactive_job(ctx->buf->conn, ctx->schema);
    db_prepare_insert(ctx->buf->conn, ctx->schema, ctx->label, ctx->stmt_name);
    ctx->prepared = 1;
    return 0;
}

// Minimal extractors for spill replay (same style as agentd).
static int json_str_field(const char *line, const char *key, char *out, size_t out_cap) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *hit = strstr(line, pat);
    if (!hit) return -1;
    hit += strlen(pat);
    size_t i = 0;
    while (*hit && *hit != '"' && i + 1 < out_cap) {
        if (*hit == '\\' && hit[1]) ++hit;
        out[i++] = *hit++;
    }
    out[i] = '\0';
    return 0;
}

static int json_num_field(const char *line, const char *key, double *out) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *hit = strstr(line, pat);
    if (!hit) return -1;
    hit += strlen(pat);
    char *endp = NULL;
    *out = strtod(hit, &endp);
    return (endp == hit) ? -1 : 0;
}

static int replay_sample(const char *line, size_t len, void *user) {
    (void)len;
    pw_flush_ctx_t *ctx = user;
    if (ensure_prepared(ctx) != 0) return -1;
    if (strncmp(line, "{\"k\":\"sample\"", 13) != 0) return 0; // skip unknown

    char pid[128] = "";
    double cpu = 0, rss = 0;
    if (json_str_field(line, "pid", pid, sizeof pid) != 0) return 0;
    json_num_field(line, "cpu", &cpu);
    json_num_field(line, "rss", &rss);
    if (db_insert_sample(ctx->buf->conn, ctx->stmt_name, pid, cpu, (long)rss) != 0)
        return -1;
    return 0;
}

static void spill_sample(pw_db_buf_t *buf, const char *pid, double cpu, long rss) {
    char epid[256], line[512];
    if (pw_json_escape(epid, sizeof epid, pid) != 0) return;
    snprintf(line, sizeof line,
             "{\"k\":\"sample\",\"pid\":\"%s\",\"cpu\":%.10g,\"rss\":%ld}",
             epid, cpu, rss);
    pw_db_buf_spill(buf, line);
}

int main(int argc, char **argv) {
    pid_t pid = -1;
    pid_t child_pid = -1;
    int interval = 2;
    int samples = -1;
    char pid_posfix[32] = "";
    {
        time_t now = time(NULL);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        strftime(pid_posfix, sizeof pid_posfix, "_%Y%m%d%H%M%S", &tm_buf);
    }
    const char *conninfo = DEFAULT_CONNINFO;
    const char *schema   = DEFAULT_SCHEMA;
    const char *label    = NULL;
    const char *command  = NULL;
    int retention_hours = PW_DEFAULT_RETENTION_HOURS;
    int inactive_hours = PW_DEFAULT_INACTIVE_HOURS;
    {
        const char *rh = getenv("PROCWATCH_RETENTION_HOURS");
        const char *ih = getenv("PROCWATCH_INACTIVE_HOURS");
        if (rh && atoi(rh) > 0) retention_hours = atoi(rh);
        if (ih && atoi(ih) > 0) inactive_hours = atoi(ih);
    }

    int opt;
    while ((opt = getopt(argc, argv, "p:i:n:d:s:l:c:R:T:h")) != -1) {
        switch (opt) {
            case 'p': pid = (pid_t)atoi(optarg); break;
            case 'i': interval = atoi(optarg); break;
            case 'n': samples  = atoi(optarg); break;
            case 'd': conninfo = optarg; break;
            case 's': schema   = optarg; break;
            case 'l': label    = optarg; break;
            case 'c': command  = optarg; break;
            case 'R': retention_hours = atoi(optarg); break;
            case 'T': inactive_hours = atoi(optarg); break;
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
    if (retention_hours <= 0 || inactive_hours <= 0) {
        fprintf(stderr, "Error: -R and -T must be positive hours.\n");
        return 1;
    }
    db_set_housekeeping(retention_hours, inactive_hours);

    if (command) {
        pid_t child = fork();
        if (child < 0) die("fork failed: %s", strerror(errno));
        if (child == 0) {
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

    char procpath[64];
    snprintf(procpath, sizeof procpath, "/proc/%d", pid);
    struct stat st;
    if (stat(procpath, &st) != 0) die("Cannot access %s: %s", procpath, strerror(errno));

    pw_db_buf_t dbbuf;
    pw_db_buf_init(&dbbuf, conninfo, NULL, "procwatch.ndjson");

    pw_flush_ctx_t fctx;
    memset(&fctx, 0, sizeof fctx);
    fctx.buf = &dbbuf;
    fctx.schema = schema;
    fctx.label = label;
    snprintf(fctx.stmt_name, sizeof fctx.stmt_name, "ins_%d", (int)pid);

    if (dbbuf.online) {
        ensure_prepared(&fctx);
        pw_db_buf_flush(&dbbuf, replay_sample, &fctx);
    }

    printf("procwatch: tracking root pid=%d (label=%s, schema=%s, retention=%dh, inactive=%dh)%s%s\n",
           pid, label, schema, db_retention_hours(), db_inactive_hours(),
           command ? " (spawned command)" : "",
           dbbuf.online ? "" : " [offline spill]");

    cpu_totals_t tot_prev = (cpu_totals_t){0}, tot_now = (cpu_totals_t){0};
    if (parse_cpu_totals(&tot_prev) != 0) die("Failed reading /proc/stat");

    prev_sample_t *prev = NULL; size_t prev_count = 0, prev_cap = 0;

    int iter = 0;
    while (samples < 0 || iter < samples) {
        sleep(interval);

        // Reconnect / flush spilled samples before this interval's writes.
        int was_offline = !dbbuf.online;
        pw_db_buf_maintain(&dbbuf, replay_sample, &fctx);
        if (dbbuf.online && (was_offline || !fctx.prepared)) {
            fctx.prepared = 0; // connection may be new; re-prepare
            ensure_prepared(&fctx);
        }

        if (stat(procpath, &st) != 0) {
            fprintf(stderr, "Root process %d exited (or access lost).\n", pid);
            break;
        }

        if (parse_cpu_totals(&tot_now) != 0) break;
        unsigned long long delta_tot  = cpu_totals_sum(&tot_now) - cpu_totals_sum(&tot_prev);

        proc_snapshot_t *snaps = NULL; size_t sn_count = 0, sn_cap = 0;
        pid_t *queue = NULL; size_t q_head = 0, q_count = 0, q_cap = 0;

        if (q_count == q_cap) { q_cap = 16; queue = realloc(queue, q_cap * sizeof(*queue)); if (!queue) die("OOM"); }
        queue[q_count++] = pid;

        while (q_head < q_count) {
            pid_t cur = queue[q_head++];

            proc_stat_t stbuf;
            if (read_proc_stat(cur, &stbuf, NULL, 0) != 0) continue;

            proc_snapshot_t snap; memset(&snap, 0, sizeof snap);
            snap.pid = cur;
            snap.ppid = (pid_t)stbuf.ppid;
            snap.stat = stbuf;
            read_proc_status(cur, &snap.status);

            if (sn_count == sn_cap) { sn_cap = sn_cap ? sn_cap*2 : 128; snaps = realloc(snaps, sn_cap * sizeof(*snaps)); if(!snaps) die("OOM"); }
            snaps[sn_count++] = snap;

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

            char pid_full[64];
            snprintf(pid_full, sizeof pid_full, "%d%s", (int)snaps[i].pid, pid_posfix);

            if (!dbbuf.online || ensure_prepared(&fctx) != 0 ||
                db_insert_sample(dbbuf.conn, fctx.stmt_name, pid_full, cpu_pct, rss_kb) != 0) {
                dbbuf.online = 0;
                spill_sample(&dbbuf, pid_full, cpu_pct, rss_kb);
            }
        }

        free(snaps);
        tot_prev = tot_now;
        ++iter;
    }

    // Final reconnect attempt to drain spill.
    pw_db_buf_maintain(&dbbuf, replay_sample, &fctx);
    pw_db_buf_close(&dbbuf);
    if (child_pid > 0) {
        int status;
        waitpid(child_pid, &status, WNOHANG);
    }
    return 0;
}
