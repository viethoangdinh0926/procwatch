// procwatch-agentd - OTLP receiver and process-metric ingest.
//
// Tables are keyed by PROCWATCH_LABEL / procwatch.label on each payload.
// Process metrics normally arrive via POST /v1/procmetrics from the inject
// thread or procwatch-wrap; optional -A enables a hostPID /proc scrape.
// When the DB is down, payloads are spilled locally and flushed on reconnect.

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
#include "../../include/db_buffer.h"
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
    pw_db_buf_t dbbuf;
    char schema[100];
    unsigned long long spans_written;
    unsigned long long spans_dropped;
    unsigned long long spans_spilled;
    unsigned long long metrics_written;
    unsigned long long metrics_spilled;
    unsigned long long payloads_traces;
    unsigned long long payloads_procmetrics;
    unsigned long long payloads_other;
} agent_state_t;

static PGconn *st_conn(agent_state_t *st) { return st->dbbuf.conn; }

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

static void mark_offline(agent_state_t *st) {
    st->dbbuf.online = 0;
    db_label_cache_reset();
}

static int write_metric(agent_state_t *st, const char *label, const char *service,
                        const char *container_id, const char *pod, int pid,
                        const char *comm, const char *runtime, double cpu,
                        long rss, long threads) {
    if (st->dbbuf.online && st_conn(st) &&
        db_ensure_label(st_conn(st), st->schema, label) == 0 &&
        db_insert_metric_labeled(st_conn(st), st->schema, label, service,
                                 container_id, pod, pid, comm, runtime, cpu,
                                 rss, threads) == 0) {
        ++st->metrics_written;
        return 0;
    }
    mark_offline(st);

    char elabel[200], esvc[512], ecomm[128], ert[64], epod[512], ecid[160];
    pw_json_escape(elabel, sizeof elabel, label);
    pw_json_escape(esvc, sizeof esvc, service ? service : "");
    pw_json_escape(ecomm, sizeof ecomm, comm ? comm : "");
    pw_json_escape(ert, sizeof ert, runtime ? runtime : "other");
    pw_json_escape(epod, sizeof epod, pod ? pod : "");
    pw_json_escape(ecid, sizeof ecid, container_id ? container_id : "");

    char line[2048];
    snprintf(line, sizeof line,
             "{\"k\":\"metric\",\"schema\":\"%s\",\"label\":\"%s\",\"service\":\"%s\","
             "\"container_id\":\"%s\",\"pod\":\"%s\",\"pid\":%d,\"comm\":\"%s\","
             "\"runtime\":\"%s\",\"cpu\":%.10g,\"rss\":%ld,\"threads\":%ld}",
             st->schema, elabel, esvc, ecid, epod, pid, ecomm, ert, cpu, rss, threads);
    if (pw_db_buf_spill(&st->dbbuf, line) == 0) ++st->metrics_spilled;
    return 0; // accepted into spill
}

static int write_span(agent_state_t *st, const pw_span_t *span) {
    if (st->dbbuf.online && st_conn(st) &&
        db_ensure_label(st_conn(st), st->schema, span->label) == 0 &&
        db_insert_span(st_conn(st), span) == 0) {
        ++st->spans_written;
        return 0;
    }
    mark_offline(st);

    char elabel[200], etrace[80], espan[40], eparent[40], ename[512];
    char ekind[40], esvc[512], escope[512], est[40], estm[512], eattr[8192];
    pw_json_escape(elabel, sizeof elabel, span->label);
    pw_json_escape(etrace, sizeof etrace, span->trace_id);
    pw_json_escape(espan, sizeof espan, span->span_id);
    pw_json_escape(eparent, sizeof eparent, span->parent_span_id);
    pw_json_escape(ename, sizeof ename, span->name);
    pw_json_escape(ekind, sizeof ekind, span->kind ? span->kind : "INTERNAL");
    pw_json_escape(esvc, sizeof esvc, span->service_name);
    pw_json_escape(escope, sizeof escope, span->scope_name);
    pw_json_escape(est, sizeof est, span->status_code ? span->status_code : "UNSET");
    pw_json_escape(estm, sizeof estm, span->status_message);
    pw_json_escape(eattr, sizeof eattr,
                   span->attributes_json[0] ? span->attributes_json : "{}");

    char ts_buf[64];
    if (span->end_ns == 0)
        snprintf(ts_buf, sizeof ts_buf, "%.6f", (double)span->start_ns / 1e9);
    else
        snprintf(ts_buf, sizeof ts_buf, "%.6f", (double)span->end_ns / 1e9);

    char line[16384];
    snprintf(line, sizeof line,
             "{\"k\":\"span\",\"schema\":\"%s\",\"label\":\"%s\",\"ts\":\"%s\","
             "\"trace_id\":\"%s\",\"span_id\":\"%s\",\"parent_span_id\":\"%s\","
             "\"name\":\"%s\",\"kind\":\"%s\",\"service_name\":\"%s\","
             "\"scope_name\":\"%s\",\"duration_ns\":%lld,\"status_code\":\"%s\","
             "\"status_message\":\"%s\",\"attributes\":\"%s\"}",
             st->schema, elabel, ts_buf, etrace, espan, eparent, ename, ekind,
             esvc, escope, (long long)span->duration_ns, est, estm, eattr);
    if (pw_db_buf_spill(&st->dbbuf, line) == 0) ++st->spans_spilled;
    return 0;
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

    if (!service[0]) snprintf(service, sizeof service, "%s", comm[0] ? comm : "unknown");
    return write_metric(st, label, service, container_id, pod, (int)pid_d, comm,
                        runtime, cpu, (long)rss, (long)threads);
}

static int span_sink(const pw_span_t *span, void *user_data) {
    agent_state_t *st = (agent_state_t *)user_data;
    if (!span->label[0] || !validate_identifier(span->label)) {
        ++st->spans_dropped;
        return 0;
    }
    return write_span(st, span);
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

static int replay_line(const char *line, size_t len, void *user) {
    agent_state_t *st = user;
    if (!st_conn(st) || PQstatus(st_conn(st)) != CONNECTION_OK) return -1;

    if (strncmp(line, "{\"k\":\"metric\"", 13) == 0) {
        char label[100] = "", service[256] = "", comm[64] = "", runtime[32] = "other";
        char pod[256] = "", container_id[80] = "";
        double pid_d = 0, cpu = 0, rss = 0, threads = 0;
        if (json_str(line, len, "label", label, sizeof label) != 0) return 0;
        if (!validate_identifier(label)) return 0;
        json_str(line, len, "service", service, sizeof service);
        json_str(line, len, "comm", comm, sizeof comm);
        json_str(line, len, "runtime", runtime, sizeof runtime);
        json_str(line, len, "pod", pod, sizeof pod);
        json_str(line, len, "container_id", container_id, sizeof container_id);
        json_num(line, len, "pid", &pid_d);
        json_num(line, len, "cpu", &cpu);
        json_num(line, len, "rss", &rss);
        json_num(line, len, "threads", &threads);
        if (db_ensure_label(st_conn(st), st->schema, label) != 0) return -1;
        if (db_insert_metric_labeled(st_conn(st), st->schema, label, service,
                                     container_id, pod, (int)pid_d, comm, runtime,
                                     cpu, (long)rss, (long)threads) != 0)
            return -1;
        ++st->metrics_written;
        return 0;
    }

    if (strncmp(line, "{\"k\":\"span\"", 11) == 0) {
        pw_span_t span;
        memset(&span, 0, sizeof span);
        char kind_buf[40] = "INTERNAL", status_buf[40] = "UNSET", ts_buf[64] = "";
        double dur = 0;
        if (json_str(line, len, "label", span.label, sizeof span.label) != 0) return 0;
        if (!validate_identifier(span.label)) return 0;
        json_str(line, len, "trace_id", span.trace_id, sizeof span.trace_id);
        json_str(line, len, "span_id", span.span_id, sizeof span.span_id);
        json_str(line, len, "parent_span_id", span.parent_span_id, sizeof span.parent_span_id);
        json_str(line, len, "name", span.name, sizeof span.name);
        json_str(line, len, "kind", kind_buf, sizeof kind_buf);
        json_str(line, len, "service_name", span.service_name, sizeof span.service_name);
        json_str(line, len, "scope_name", span.scope_name, sizeof span.scope_name);
        json_str(line, len, "status_code", status_buf, sizeof status_buf);
        json_str(line, len, "status_message", span.status_message, sizeof span.status_message);
        json_str(line, len, "attributes", span.attributes_json, sizeof span.attributes_json);
        json_str(line, len, "ts", ts_buf, sizeof ts_buf);
        if (!span.attributes_json[0])
            snprintf(span.attributes_json, sizeof span.attributes_json, "{}");
        json_num(line, len, "duration_ns", &dur);
        span.kind = kind_buf;
        span.status_code = status_buf;
        span.duration_ns = (int64_t)dur;
        double ts = ts_buf[0] ? strtod(ts_buf, NULL) : 0;
        span.end_ns = (uint64_t)(ts * 1e9);
        span.start_ns = span.end_ns;
        if (db_ensure_label(st_conn(st), st->schema, span.label) != 0) return -1;
        if (db_insert_span(st_conn(st), &span) != 0) return -1;
        ++st->spans_written;
        return 0;
    }
    return 0; // skip unknown
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [-b bind_addr] [-P port] [-i interval_sec]\n"
        "          [-d conn_str] [-s schema] [-R retention_hours] [-T inactive_hours]\n"
        "          [-A] [-h]\n"
        "  -b <addr>     Listen address (default 0.0.0.0, env PROCWATCH_BIND)\n"
        "  -P <port>     OTLP/HTTP port (default 4318, env PROCWATCH_PORT)\n"
        "  -i <seconds>  Host-scrape interval when -A is set (default 10)\n"
        "  -d <conn>     PostgreSQL connection string (env PROCWATCH_DB)\n"
        "  -s <schema>   Schema (default procwatch, env PROCWATCH_SCHEMA)\n"
        "  -R <hours>    Timescale chunk retention (default %d, env PROCWATCH_RETENTION_HOURS)\n"
        "  -T <hours>    Drop tables idle longer than this (default %d, env PROCWATCH_INACTIVE_HOURS)\n"
        "  -A            Enable optional hostPID /proc scrape for labeled procs\n"
        "\n"
        "Tables are named <label>_spans / <label>_procs from each payload's\n"
        "PROCWATCH_LABEL / procwatch.label; no -l flag is required.\n"
        "If the database is unreachable, data is spilled under PROCWATCH_SPILL_DIR\n"
        "(default /var/tmp/procwatch) and flushed on reconnect.\n",
        argv0, PW_DEFAULT_RETENTION_HOURS, PW_DEFAULT_INACTIVE_HOURS);
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
    int retention_hours = env_int("PROCWATCH_RETENTION_HOURS", PW_DEFAULT_RETENTION_HOURS);
    int inactive_hours = env_int("PROCWATCH_INACTIVE_HOURS", PW_DEFAULT_INACTIVE_HOURS);

    int opt;
    while ((opt = getopt(argc, argv, "b:P:i:d:s:R:T:Ah")) != -1) {
        switch (opt) {
            case 'b': bind_addr = optarg; break;
            case 'P': port = atoi(optarg); break;
            case 'i': interval = atoi(optarg); break;
            case 'd': conninfo = optarg; break;
            case 's': schema = optarg; break;
            case 'R': retention_hours = atoi(optarg); break;
            case 'T': inactive_hours = atoi(optarg); break;
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
    if (retention_hours <= 0 || inactive_hours <= 0) {
        fprintf(stderr, "Error: -R and -T must be positive hours.\n");
        return 1;
    }
    db_set_housekeeping(retention_hours, inactive_hours);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    agent_state_t state;
    memset(&state, 0, sizeof state);
    snprintf(state.schema, sizeof state.schema, "%s", schema);
    pw_db_buf_init(&state.dbbuf, conninfo, NULL, "agentd.ndjson");

    if (state.dbbuf.online) {
        db_ensure_schema(st_conn(&state), schema);
        db_try_enable_timescaledb(st_conn(&state));
        db_ensure_drop_inactive_job(st_conn(&state), schema);
        pw_db_buf_flush(&state.dbbuf, replay_line, &state);
    }

    pw_http_server_t server;
    if (http_server_init(&server, bind_addr, port, on_request, &state) != 0) {
        pw_db_buf_close(&state.dbbuf);
        return 1;
    }

    pw_collector_t collector;
    memset(&collector, 0, sizeof collector);
    if (host_collect) collector_init(&collector, schema);

    printf("procwatch-agentd: OTLP/HTTP on %s:%d, schema=%s (dynamic labels)%s\n",
           bind_addr, port, schema,
           state.dbbuf.online ? "" : " [offline spill]");
    printf("procwatch-agentd: retention=%dh inactive=%dh; host scrape %s\n",
           db_retention_hours(), db_inactive_hours(),
           host_collect ? "enabled (-A)" : "disabled (inject/wrap push)");
    fflush(stdout);

    time_t next_tick = time(NULL) + interval;
    time_t next_maintain = time(NULL) + 1;

    while (!g_stop) {
        time_t now = time(NULL);
        int timeout_ms = 1000;
        if (host_collect) {
            long remaining = (long)(next_tick - now) * 1000;
            timeout_ms = (remaining > 0) ? (int)remaining : 0;
            if (timeout_ms > 1000) timeout_ms = 1000;
        }

        http_server_tick(&server, timeout_ms);
        now = time(NULL);

        if (now >= next_maintain) {
            int was_offline = !state.dbbuf.online;
            pw_db_buf_maintain(&state.dbbuf, replay_line, &state);
            if (state.dbbuf.online && was_offline) {
                db_label_cache_reset();
                db_ensure_schema(st_conn(&state), schema);
                db_try_enable_timescaledb(st_conn(&state));
                db_ensure_drop_inactive_job(st_conn(&state), schema);
                // Flush again after schema/job setup (maintain may have no-oped
                // inserts that needed labels first).
                pw_db_buf_flush(&state.dbbuf, replay_line, &state);
            }
            next_maintain = now + 1;
        }

        if (host_collect && now >= next_tick) {
            if (state.dbbuf.online && st_conn(&state) &&
                PQstatus(st_conn(&state)) == CONNECTION_OK)
                collector_tick(&collector, st_conn(&state));
            next_tick = time(NULL) + interval;
        }
    }

    pw_db_buf_maintain(&state.dbbuf, replay_line, &state);

    printf("procwatch-agentd: shutting down (%llu spans, %llu metrics, "
           "%llu spilled spans, %llu spilled metrics, %llu host samples)\n",
           state.spans_written, state.metrics_written,
           state.spans_spilled, state.metrics_spilled, collector.samples_written);

    http_server_close(&server);
    if (host_collect) collector_free(&collector);
    pw_db_buf_close(&state.dbbuf);
    return 0;
}
