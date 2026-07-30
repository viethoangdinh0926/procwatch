#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../include/util.h"
#include "../include/proc_scan.h"

int parse_cpu_totals(cpu_totals_t *t) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[1024];
    if (!fgets(line, sizeof line, f)) { fclose(f); return -1; }
    fclose(f);
    memset(t, 0, sizeof(*t));
    char *p = line;
    if (!starts_with(p, "cpu")) return -1;
    char *save = NULL;
    strtok_r(p, " ", &save); // "cpu"
    unsigned long long v[10] = {0};
    for (int i = 0; i < 10; ++i) {
        char *tok = strtok_r(NULL, " ", &save);
        if (!tok) break;
        v[i] = strtoull(tok, NULL, 10);
    }
    t->user = v[0];
    t->nice = v[1];
    t->system = v[2];
    t->idle = v[3];
    t->iowait = v[4];
    t->irq = v[5];
    t->softirq = v[6];
    t->steal = v[7];
    return 0;
}

unsigned long long cpu_totals_sum(const cpu_totals_t *t) {
    return t->user + t->nice + t->system + t->idle + t->iowait + t->irq + t->softirq + t->steal;
}

int read_proc_stat(pid_t pid, proc_stat_t *st, char *comm_buf, size_t comm_sz) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[4096];
    if (!fgets(line, sizeof line, f)) { fclose(f); return -1; }
    fclose(f);

    char *rparen = strrchr(line, ')');
    if (!rparen) return -1;

    char *lparen = strchr(line, '(');
    if (!lparen || lparen > rparen) return -1;
    size_t clen = rparen - lparen - 1;
    if (comm_buf && comm_sz) {
        size_t copy = (clen < comm_sz - 1) ? clen : (comm_sz - 1);
        memcpy(comm_buf, lparen + 1, copy);
        comm_buf[copy] = '\0';
    }

    const char *rest = rparen + 2; // skip ") "
    char state;
    unsigned long ppid, pgrp, session, tty_nr, tpgid, flags;
    unsigned long long cminflt, cmajflt, cutime, cstime, itrealvalue;
    memset(st, 0, sizeof(*st));

    int n = sscanf(rest,
        "%c %lu %lu %lu %lu %lu %lu "
        "%llu %llu %llu %llu %llu %llu "
        "%llu %llu %ld %ld %ld %llu %llu %ld %ld",
        &state, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
        &st->minflt, &cminflt, &st->majflt, &cmajflt, &st->utime, &st->stime,
        &cutime, &cstime, &st->priority, &st->nice, &st->num_threads,
        &itrealvalue, &st->starttime, &st->vsize, &st->rss);

    st->ppid = ppid;
    if (n < 22) return -1;
    return 0;
}

int read_proc_status(pid_t pid, proc_status_t *ps) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(ps, 0, sizeof(*ps));
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (starts_with(line, "VmRSS:")) {
            ps->vmrss_kb = strtol(line + 6, NULL, 10);
        } else if (starts_with(line, "VmSize:")) {
            ps->vmsize_kb = strtol(line + 7, NULL, 10);
        } else if (starts_with(line, "VmSwap:")) {
            ps->vmswap_kb = strtol(line + 7, NULL, 10);
        } else if (starts_with(line, "Threads:")) {
            ps->threads = strtol(line + 8, NULL, 10);
        } else if (starts_with(line, "voluntary_ctxt_switches:")) {
            ps->vctx = strtoull(line + 24, NULL, 10);
        } else if (starts_with(line, "nonvoluntary_ctxt_switches:")) {
            ps->nvctx = strtoull(line + 27, NULL, 10);
        }
    }
    fclose(f);
    return 0;
}

int read_proc_smaps_rollup(pid_t pid, proc_smaps_rollup_t *sr) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/smaps_rollup", pid);
    FILE *f = fopen(path, "r");
    if (!f) { sr->have_rollup = 0; return -1; }
    char line[256];
    sr->have_rollup = 1;
    sr->rss_kb = sr->pss_kb = sr->swap_kb = -1;
    while (fgets(line, sizeof line, f)) {
        if (starts_with(line, "Rss:"))      sr->rss_kb  = strtol(line + 4, NULL, 10);
        else if (starts_with(line, "Pss:")) sr->pss_kb  = strtol(line + 4, NULL, 10);
        else if (starts_with(line, "Swap:")) sr->swap_kb = strtol(line + 5, NULL, 10);
    }
    fclose(f);
    return 0;
}

int read_proc_io(pid_t pid, proc_io_t *pio) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/io", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(pio, 0, sizeof(*pio));
    char key[64];
    unsigned long long val;
    while (fscanf(f, "%63s %llu", key, &val) == 2) {
        if (strcmp(key, "rchar:") == 0) pio->rchar = val;
        else if (strcmp(key, "wchar:") == 0) pio->wchar = val;
        else if (strcmp(key, "syscr:") == 0) pio->syscr = val;
        else if (strcmp(key, "syscw:") == 0) pio->syscw = val;
        else if (strcmp(key, "read_bytes:") == 0) pio->read_bytes = val;
        else if (strcmp(key, "write_bytes:") == 0) pio->write_bytes = val;
        else if (strcmp(key, "cancelled_write_bytes:") == 0) pio->cancelled_write_bytes = val;
    }
    fclose(f);
    return 0;
}

int count_fds(pid_t pid, int *fd_count, int *socket_count) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/fd", pid);
    DIR *d = opendir(path);
    if (!d) return -1;
    int fds = 0, skts = 0;
    struct dirent *de;
    char linkpath[PATH_MAX];
    char target[PATH_MAX];
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        ++fds;
        snprintf(linkpath, sizeof linkpath, "%s/%s", path, de->d_name);
        ssize_t n = readlink(linkpath, target, sizeof target - 1);
        if (n > 0) {
            target[n] = '\0';
            if (starts_with(target, "socket:[")) ++skts;
        }
    }
    closedir(d);
    if (fd_count) *fd_count = fds;
    if (socket_count) *socket_count = skts;
    return 0;
}

int read_first_line(const char *path, char *buf, size_t bufsz) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, bufsz, f)) { fclose(f); return -1; }
    fclose(f);
    size_t n = strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    return 0;
}

int read_cgroup(pid_t pid, char *buf, size_t bufsz) {
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/cgroup", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (fgets(line, sizeof line, f)) {
        size_t len = strnlen(line, sizeof line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        strncpy(buf, line, bufsz - 1);
        buf[bufsz - 1] = '\0';
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

int read_limits_excerpt(pid_t pid, long *max_fds_soft, long *max_fds_hard, long *stack_soft, long *stack_hard) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/limits", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    fgets(line, sizeof line, f); // header
    long mfs=-1, mfh=-1, ss=-1, sh=-1;
    while (fgets(line, sizeof line, f)) {
        if (starts_with(line, "Max open files")) {
            char soft[64], hard[64];
            if (sscanf(line, "Max open files%63s%63s", soft, hard) == 2) {
                mfs = (strcmp(soft, "unlimited")==0) ? -1 : atol(soft);
                mfh = (strcmp(hard, "unlimited")==0) ? -1 : atol(hard);
            }
        } else if (starts_with(line, "Max stack size")) {
            char soft[64], hard[64];
            if (sscanf(line, "Max stack size%63s%63s", soft, hard) == 2) {
                ss = (strcmp(soft, "unlimited")==0) ? -1 : atol(soft);
                sh = (strcmp(hard, "unlimited")==0) ? -1 : atol(hard);
            }
        }
    }
    fclose(f);
    if (max_fds_soft) *max_fds_soft = mfs;
    if (max_fds_hard) *max_fds_hard = mfh;
    if (stack_soft) *stack_soft = ss;
    if (stack_hard) *stack_hard = sh;
    return 0;
}

static int pid_list_contains(pid_t pid, pid_t *list, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (list[i] == pid) return 1;
    }
    return 0;
}

int read_children_pids(pid_t pid, pid_t **out, size_t *out_count, size_t *out_cap) {
    char taskdir[64];
    snprintf(taskdir, sizeof taskdir, "/proc/%d/task", pid);

    DIR *d = opendir(taskdir);
    if (!d) return -1;

    struct dirent *de;
    char path[128];
    char line[4096];
    int found_any = 0;

    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        // only consider numeric TIDs
        char *endptr = NULL;
        long tid = strtol(de->d_name, &endptr, 10);
        if (!endptr || *endptr != '\0' || tid <= 0) continue;

        snprintf(path, sizeof path, "%s/%ld/children", taskdir, tid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(line, sizeof line, f)) { fclose(f); continue; }
        fclose(f);
        found_any = 1;

        char *save = NULL;
        char *tok = strtok_r(line, " \n\t", &save);
        while (tok) {
            pid_t cpid = (pid_t)strtol(tok, NULL, 10);
            if (cpid > 0 && !pid_list_contains(cpid, *out, *out_count)) {
                if (*out_count == *out_cap) {
                    *out_cap = *out_cap ? (*out_cap * 2) : 16;
                    pid_t *tmp = realloc(*out, (*out_cap) * sizeof(pid_t));
                    if (!tmp) { closedir(d); return -1; }
                    *out = tmp;
                }
                (*out)[(*out_count)++] = cpid;
            }
            tok = strtok_r(NULL, " \n\t", &save);
        }
    }

    closedir(d);
    return found_any ? 0 : -1;
}

int pid_seen(pid_t pid, pid_t *queue, size_t q_count, proc_snapshot_t *snaps, size_t sn_count) {
    for (size_t i = 0; i < q_count; ++i) if (queue[i] == pid) return 1;
    for (size_t i = 0; i < sn_count; ++i) if (snaps[i].pid == pid) return 1;
    return 0;
}
