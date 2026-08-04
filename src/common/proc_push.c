// libc-only process sampling + raw HTTP POST for inject and wrap.

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "../include/proc_push.h"

int pw_label_valid(const char *label) {
    if (!label || !*label) return 0;
    size_t n = strlen(label);
    if (n == 0 || n >= PW_LABEL_MAX) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)label[i];
        if (!(isalnum(c) || c == '_')) return 0;
    }
    return 1;
}

static int read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return 0;
}

static unsigned long long read_cpu_total(void) {
    char buf[1024];
    if (read_file("/proc/stat", buf, sizeof buf) != 0) return 0;
    if (strncmp(buf, "cpu ", 4) != 0 && strncmp(buf, "cpu\t", 4) != 0) return 0;
    char *p = buf + 3;
    while (*p == ' ' || *p == '\t') ++p;
    unsigned long long sum = 0, v;
    for (int i = 0; i < 10; ++i) {
        char *end = NULL;
        v = strtoull(p, &end, 10);
        if (end == p) break;
        sum += v;
        p = end;
    }
    return sum;
}

static int parse_stat(pid_t pid, unsigned long long *utime, unsigned long long *stime,
                      long *rss_pages, long *threads, char *comm, size_t comm_cap) {
    char path[64], line[4096];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    if (read_file(path, line, sizeof line) != 0) return -1;

    char *lparen = strchr(line, '(');
    char *rparen = strrchr(line, ')');
    if (!lparen || !rparen || rparen <= lparen) return -1;
    size_t clen = (size_t)(rparen - lparen - 1);
    if (clen >= comm_cap) clen = comm_cap - 1;
    memcpy(comm, lparen + 1, clen);
    comm[clen] = '\0';

    // Fields after ") ": state(3) ... utime(14) stime(15) ... num_threads(20) ... rss(24)
    char *p = rparen + 2;
    unsigned long long fields[30] = {0};
    for (int i = 0; i < 30; ++i) {
        char *end = NULL;
        while (*p == ' ') ++p;
        fields[i] = strtoull(p, &end, 10);
        if (end == p) break;
        p = end;
    }
    *utime = fields[11];   // field 14 -> index 11 after comm
    *stime = fields[12];
    *threads = (long)fields[17];
    *rss_pages = (long)fields[21];
    return 0;
}

static void parse_status(pid_t pid, long *rss_kb, long *threads) {
    char path[64], buf[4096];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    if (read_file(path, buf, sizeof buf) != 0) return;
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "VmRSS:", 6) == 0) {
            *rss_kb = strtol(line + 6, NULL, 10);
        } else if (strncmp(line, "Threads:", 8) == 0) {
            *threads = strtol(line + 8, NULL, 10);
        }
        if (!nl) break;
        line = nl + 1;
    }
}

int pw_sample_pid(pid_t pid, pw_proc_baseline_t *base, pw_proc_sample_t *out) {
    unsigned long long utime = 0, stime = 0;
    long rss_pages = 0, threads = 1;
    char comm[PW_PUSH_COMM_MAX];
    if (parse_stat(pid, &utime, &stime, &rss_pages, &threads, comm, sizeof comm) != 0)
        return -1;

    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (page_kb <= 0) page_kb = 4;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu <= 0) ncpu = 1;

    long rss_kb = rss_pages * page_kb;
    parse_status(pid, &rss_kb, &threads);

    unsigned long long proc_time = utime + stime;
    unsigned long long cpu_total = read_cpu_total();

    double cpu_pct = 0.0;
    if (base->have_baseline && cpu_total > base->cpu_total && proc_time >= base->proc_time) {
        unsigned long long d_proc = proc_time - base->proc_time;
        unsigned long long d_cpu = cpu_total - base->cpu_total;
        if (d_cpu > 0)
            cpu_pct = (100.0 * (double)d_proc / (double)d_cpu) * (double)ncpu;
    }

    base->proc_time = proc_time;
    base->cpu_total = cpu_total;
    base->have_baseline = 1;

    out->pid = (int)pid;
    if (!out->pid_key[0])
        snprintf(out->pid_key, sizeof out->pid_key, "%d", (int)pid);
    snprintf(out->comm, sizeof out->comm, "%s", comm);
    out->cpu_pct = cpu_pct;
    out->rss_kb = rss_kb;
    out->threads = threads;
    return 0;
}

void pw_format_created_stamp(char *out, size_t cap, time_t when) {
    if (!out || cap < 2) return;
    struct tm tm_buf;
    localtime_r(&when, &tm_buf);
    // Leading underscore so Grafana/series keys stay "<pid>_<stamp>".
    strftime(out, cap, "_%Y%m%d%H%M%S", &tm_buf);
}

void pw_sample_set_pid_key(pw_proc_sample_t *out, int pid, const char *stamp) {
    if (!out) return;
    out->pid = pid;
    if (stamp && *stamp)
        snprintf(out->pid_key, sizeof out->pid_key, "%d%s", pid, stamp);
    else
        snprintf(out->pid_key, sizeof out->pid_key, "%d", pid);
}

void pw_sample_fill_identity(pw_proc_sample_t *out, const char *runtime_hint) {
    const char *label = getenv("PROCWATCH_LABEL");
    if (label) snprintf(out->label, sizeof out->label, "%s", label);
    else out->label[0] = '\0';

    const char *svc = getenv("OTEL_SERVICE_NAME");
    if (!svc || !*svc) svc = getenv("PROCWATCH_SERVICE");
    if (svc && *svc) snprintf(out->service, sizeof out->service, "%s", svc);
    else snprintf(out->service, sizeof out->service, "%s", out->comm);

    const char *pod = getenv("PROCWATCH_POD");
    if (pod) snprintf(out->pod, sizeof out->pod, "%s", pod);
    else out->pod[0] = '\0';

    out->container_id[0] = '\0';

    if (runtime_hint && *runtime_hint)
        snprintf(out->runtime, sizeof out->runtime, "%s", runtime_hint);
    else
        snprintf(out->runtime, sizeof out->runtime, "other");
}

static int parse_http_endpoint(const char *endpoint, char *host, size_t host_cap,
                               int *port, char *path_prefix, size_t path_cap) {
    if (!endpoint || !*endpoint) return -1;
    const char *p = endpoint;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) return -1; // TLS not supported

    const char *slash = strchr(p, '/');
    const char *colon = NULL;
    for (const char *q = p; q != slash && *q; ++q) {
        if (*q == ':') { colon = q; break; }
    }

    size_t host_len;
    if (colon) {
        host_len = (size_t)(colon - p);
        *port = atoi(colon + 1);
    } else if (slash) {
        host_len = (size_t)(slash - p);
        *port = 80;
    } else {
        host_len = strlen(p);
        *port = 80;
    }
    if (host_len == 0 || host_len >= host_cap) return -1;
    memcpy(host, p, host_len);
    host[host_len] = '\0';
    if (*port <= 0 || *port > 65535) *port = 80;

    if (slash && slash[1]) {
        snprintf(path_prefix, path_cap, "%s", slash);
        size_t n = strlen(path_prefix);
        while (n > 0 && path_prefix[n - 1] == '/') path_prefix[--n] = '\0';
    } else {
        path_prefix[0] = '\0';
    }
    return 0;
}

int pw_push_sample(const char *endpoint, const pw_proc_sample_t *s) {
    if (!s || !pw_label_valid(s->label)) return -1;

    char host[256], path_prefix[256];
    int port = 4318;
    if (parse_http_endpoint(endpoint && *endpoint ? endpoint : "http://127.0.0.1:4318",
                            host, sizeof host, &port, path_prefix, sizeof path_prefix) != 0)
        return -1;

    char body[1024];
    const char *pid_field = (s->pid_key[0]) ? s->pid_key : "";
    char pid_fallback[32];
    if (!pid_field[0]) {
        snprintf(pid_fallback, sizeof pid_fallback, "%d", s->pid);
        pid_field = pid_fallback;
    }
    int blen = snprintf(body, sizeof body,
        "{\"label\":\"%s\",\"service\":\"%s\",\"pid\":\"%s\",\"comm\":\"%s\","
        "\"cpu_pct\":%.6f,\"rss_kb\":%ld,\"threads\":%ld,\"runtime\":\"%s\","
        "\"pod\":\"%s\",\"container_id\":\"%s\"}",
        s->label, s->service, pid_field, s->comm,
        s->cpu_pct, s->rss_kb, s->threads, s->runtime,
        s->pod, s->container_id);
    if (blen <= 0 || (size_t)blen >= sizeof body) return -1;

    char path[320];
    snprintf(path, sizeof path, "%s/v1/procmetrics", path_prefix);

    char req[1536];
    int rlen = snprintf(req, sizeof req,
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, port, blen, body);
    if (rlen <= 0 || (size_t)rlen >= sizeof req) return -1;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portbuf[16];
    snprintf(portbuf, sizeof portbuf, "%d", port);
    if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    size_t off = 0;
    while (off < (size_t)rlen) {
        ssize_t n = send(fd, req + off, (size_t)rlen - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }

    char resp[256];
    ssize_t n = recv(fd, resp, sizeof resp - 1, 0);
    close(fd);
    if (n <= 0) return -1;
    resp[n] = '\0';
    // "HTTP/1.1 2xx"
    char *sp = strchr(resp, ' ');
    if (!sp) return -1;
    int code = atoi(sp + 1);
    return (code >= 200 && code < 300) ? 0 : -1;
}
