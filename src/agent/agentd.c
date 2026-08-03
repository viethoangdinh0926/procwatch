// procwatch-agentd - OTLP receiver and process-metric ingest.
//
// Tables are keyed by PROCWATCH_LABEL / procwatch.label on each payload.
// Process metrics normally arrive via POST /v1/procmetrics from the inject
// thread or procwatch-wrap; optional -A enables a hostPID /proc scrape.

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libpq-fe.h>

#include "../../include/util.h"
#include "../../include/db.h"
#include "../../include/agent/http_server.h"
#include "../../include/agent/otlp.h"
#include "../../include/agent/db_otlp.h"
#include "../../include/agent/collector.h"

static const char *DEFAULT_CONNINFO =
    "postgresql://procwatcher:procwatcherpw@127.0.0.1:5433/procwatcherdb";
static const char *DEFAULT_SCHEMA = "procwatch";

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

typedef struct {
    PGconn *db;
    char schema[100];
    unsigned long long spans_written;
    unsigned long long spans_dropped;
    unsigned long long metrics_written;
    unsigned long long payloads_traces;
    unsigned long long payloads_procmetrics;
    unsigned long long payloads_other;
} agent_state_t;

// Minimal JSON string extractor: finds "key":"value" or "key":number.
static int json_str(const char *body, size_t len, const char *key,
                    char *out, size_t out_cap) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *end = body + len;
    const char *p = body;
    while (p < end) {
        const char *hit = strstr(p, pat);
        if (!hit || hit >= end) return -1;
        const char *c = hit + strlen(pat);
        while (c < end && (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')) ++c;
        if (c >= end || *c != ':') { p = hit + 1; continue; }
        ++c;
        while (c < end && (*c == ' ' || *c == '\t')) ++c;
        if (c >= end || *c != '"') { p = hit + 1; continue; }
        ++c;
        size_t i = 0;
        while (c < end && *c != '"' && i + 1 < out_cap) {
            if (*c == '\\' && c + 1 < end) { ++c; }
            out[i++] = *c++;
        }
        out[i] = '\0';
        return 0;
    }
    return -1;
}

static int json_num(const char *body, size_t len, const char *key, double *out) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *end = body + len;
    const char *p = body;
    while (p < end) {
        const char *hit = strstr(p, pat);
        if (!hit || hit >= end) return -1;
        const char *c = hit + strlen(pat);
        while (c < end && (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')) ++c;
        if (c >= end || *c != ':') { p = hit + 1; continue; }
        ++c;
        while (c < end && (*c == ' ' || *c == '\t')) ++c;
        char *endp = NULL;
        *out = strtod(c, &endp);
        if (endp == c) { p = hit + 1; continue; }
        return 0;
    }
    return -1;
}

static int handle_procmetrics(agent_state_t *st, const uint8_t *body, size_t len) {
    char label[100] = "", service[256] = "", comm[64] = "", runtime[32] = "other";
    char pod[256] = "", container_id[80] = "";
    double pid_d = 0, cpu = 0, rss = 0, threads = 0;

    if (json_str((const char *)body, len, "label", label, sizeof label) != 0)
        return -1;
    if (!validate_identifier(label)) return -1;

    json_str((const char *)body, len, "service", service, sizeof service);
    json_str((const char *)body, len, "comm", comm, sizeof comm);
    json_str((const char *)body, len, "runtime", runtime, sizeof runtime);
    json_str((const char *)body, len, "pod", pod, sizeof pod);
    json_str((const char *)body, len, "container_id", container_id, sizeof container_id);
    json_num((const char *)body, len, "pid", &pid_d);
    json_num((const char *)body, len, "cpu_pct", &cpu);
    json_num((const char *)body, len, "rss_kb", &rss);
    json_num((const char *)body, len, "threads", &threads);

    if (db_ensure_label(st->db, st->schema, label) != 0) return -1;
    if (!service[0]) snprintf(service, sizeof service, "%s", comm[0] ? comm : "unknown");

    if (db_insert_metric_labeled(st->db, st->schema, label, service, container_id, pod,
                                 (int)pid_d, comm, runtime, cpu, (long)rss,
                                 (long)threads) == 0) {
        ++st->metrics_written;
        return 0;
    }
    return -1;
}

static int span_sink(const pw_span_t *span, void *user_data) {
    agent_state_t *st = (agent_state_t *)user_data;
    if (!span->label[0] || !validate_identifier(span->label)) {
        ++st->spans_dropped;
        return 0; // skip unlabeled; keep decoding the rest
    }
    if (db_ensure_label(st->db, st->schema, span->label) != 0) {
        ++st->spans_dropped;
        return 0;
    }
    if (db_insert_span(st->db, span) == 0) ++st->spans_written;
    else ++st->spans_dropped;
    return 0;
}

static int on_request(const pw_http_request_t *req, void *user_data) {
    agent_state_t *st = (agent_state_t *)user_data;

    if (req->signal == PW_SIGNAL_PROCMETRICS) {
        ++st->payloads_procmetrics;
        return handle_procmetrics(st, req->body, req->body_len);
    }

    if (req->signal != PW_SIGNAL_TRACES) {
        ++st->payloads_other;
        return 0;
    }

    ++st->payloads_traces;
    int n = otlp_decode_traces(req->body, req->body_len, span_sink, st);
    if (n < 0) {
        fprintf(stderr, "malformed OTLP trace payload (%zu bytes)\n", req->body_len);
        return -1;
    }
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [-b bind_addr] [-P port] [-i interval_sec]\n"
        "          [-d conn_str] [-s schema] [-A] [-h]\n"
        "  -b <addr>     Listen address (default 0.0.0.0, env PROCWATCH_BIND)\n"
        "  -P <port>     OTLP/HTTP port (default 4318, env PROCWATCH_PORT)\n"
        "  -i <seconds>  Host-scrape interval when -A is set (default 10)\n"
        "  -d <conn>     PostgreSQL connection string (env PROCWATCH_DB)\n"
        "  -s <schema>   Schema (default procwatch, env PROCWATCH_SCHEMA)\n"
        "  -A            Enable optional hostPID /proc scrape for labeled procs\n"
        "\n"
        "Tables are named <label>_spans / <label>_procs from each payload's\n"
        "PROCWATCH_LABEL / procwatch.label; no -l flag is required.\n",
        argv0);
}

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}

static int env_int(const char *name, int fallback) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    int n = atoi(v);
    return n > 0 ? n : fallback;
}

int main(int argc, char **argv) {
    const char *bind_addr = env_or("PROCWATCH_BIND", "0.0.0.0");
    const char *conninfo = env_or("PROCWATCH_DB", DEFAULT_CONNINFO);
    const char *schema = env_or("PROCWATCH_SCHEMA", DEFAULT_SCHEMA);
    int port = env_int("PROCWATCH_PORT", 4318);
    int interval = env_int("PROCWATCH_INTERVAL", 10);
    int host_collect = getenv("PROCWATCH_HOST_COLLECT") != NULL;

    int opt;
    while ((opt = getopt(argc, argv, "b:P:i:d:s:Ah")) != -1) {
        switch (opt) {
            case 'b': bind_addr = optarg; break;
            case 'P': port = atoi(optarg); break;
            case 'i': interval = atoi(optarg); break;
            case 'd': conninfo = optarg; break;
            case 's': schema = optarg; break;
            case 'A': host_collect = 1; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    if (!validate_identifier(schema)) {
        fprintf(stderr, "Error: schema must match ^[A-Za-z0-9_]+$ and be <100 chars.\n");
        return 1;
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: port must be 1-65535.\n");
        return 1;
    }
    if (interval <= 0) interval = 10;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    agent_state_t state;
    memset(&state, 0, sizeof state);
    snprintf(state.schema, sizeof state.schema, "%s", schema);
    state.db = db_connect_or_die(conninfo);

    db_ensure_schema(state.db, schema);
    db_try_enable_timescaledb(state.db);
    db_ensure_drop_inactive_job(state.db, schema);

    pw_http_server_t server;
    if (http_server_init(&server, bind_addr, port, on_request, &state) != 0) {
        PQfinish(state.db);
        return 1;
    }

    pw_collector_t collector;
    memset(&collector, 0, sizeof collector);
    if (host_collect) collector_init(&collector, schema);

    printf("procwatch-agentd: OTLP/HTTP on %s:%d, schema=%s (dynamic labels)\n",
           bind_addr, port, schema);
    printf("procwatch-agentd: host scrape %s\n",
           host_collect ? "enabled (-A)" : "disabled (inject/wrap push)");
    fflush(stdout);

    time_t next_tick = time(NULL) + interval;

    while (!g_stop) {
        time_t now = time(NULL);
        int timeout_ms = 1000;
        if (host_collect) {
            long remaining = (long)(next_tick - now) * 1000;
            timeout_ms = (remaining > 0) ? (int)remaining : 0;
            if (timeout_ms > 1000) timeout_ms = 1000;
        }

        http_server_tick(&server, timeout_ms);

        if (host_collect && time(NULL) >= next_tick) {
            if (PQstatus(state.db) != CONNECTION_OK) {
                fprintf(stderr, "database connection lost, resetting\n");
                PQreset(state.db);
                db_label_cache_reset();
            }
            if (PQstatus(state.db) == CONNECTION_OK)
                collector_tick(&collector, state.db);
            next_tick = time(NULL) + interval;
        }
    }

    printf("procwatch-agentd: shutting down (%llu spans, %llu metrics pushed, "
           "%llu host samples)\n",
           state.spans_written, state.metrics_written, collector.samples_written);

    http_server_close(&server);
    if (host_collect) collector_free(&collector);
    PQfinish(state.db);
    return 0;
}
