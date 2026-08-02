// procwatch-agentd - OTLP receiver and process metric collector.
//
// Runs one event loop: poll() services the OTLP/HTTP receiver, and its
// timeout doubles as the collector's tick. Single-threaded by design, so the
// libpq connection needs no locking and spans and metrics are written from
// the same place.
//
// The procwatch binary is unaffected by any of this; agentd is a separate
// executable that only shares the /proc parsing and libpq helpers.

#define _GNU_SOURCE
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
static const char *DEFAULT_LABEL = "otel";

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

typedef struct {
    PGconn *db;
    unsigned long long spans_written;
    unsigned long long spans_dropped;
    unsigned long long payloads_traces;
    unsigned long long payloads_other;
} agent_state_t;

static int span_sink(const pw_span_t *span, void *user_data) {
    agent_state_t *st = (agent_state_t *)user_data;
    if (db_insert_span(st->db, span) == 0) ++st->spans_written;
    else ++st->spans_dropped;
    return 0;
}

static int on_request(const pw_http_request_t *req, void *user_data) {
    agent_state_t *st = (agent_state_t *)user_data;

    if (req->signal != PW_SIGNAL_TRACES) {
        // Metrics and logs are acknowledged but not stored. An SDK told to
        // export all three signals would otherwise log connection errors
        // every interval, which looks like a broken agent.
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
        "Usage: %s [-b bind_addr] [-P port] [-i interval_sec] [-l label]\n"
        "          [-d conn_str] [-s schema] [-A] [-M] [-h]\n"
        "  -b <addr>     Listen address (default 0.0.0.0, env PROCWATCH_BIND)\n"
        "  -P <port>     OTLP/HTTP port (default 4318, env PROCWATCH_PORT)\n"
        "  -i <seconds>  Process metric interval (default 10, env PROCWATCH_INTERVAL)\n"
        "  -l <label>    Table prefix; creates <label>_spans and <label>_procs\n"
        "                (default otel, env PROCWATCH_LABEL)\n"
        "  -d <conn>     PostgreSQL connection string (env PROCWATCH_DB)\n"
        "  -s <schema>   Schema (default procwatch, env PROCWATCH_SCHEMA)\n"
        "  -A            Collect every process, not just injected ones\n"
        "  -M            Disable process metric collection (receiver only)\n",
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
    const char *label = env_or("PROCWATCH_LABEL", DEFAULT_LABEL);
    int port = env_int("PROCWATCH_PORT", 4318);
    int interval = env_int("PROCWATCH_INTERVAL", 10);
    int collect_all = getenv("PROCWATCH_COLLECT_ALL") != NULL;
    int metrics_enabled = 1;

    int opt;
    while ((opt = getopt(argc, argv, "b:P:i:l:d:s:AMh")) != -1) {
        switch (opt) {
            case 'b': bind_addr = optarg; break;
            case 'P': port = atoi(optarg); break;
            case 'i': interval = atoi(optarg); break;
            case 'l': label = optarg; break;
            case 'd': conninfo = optarg; break;
            case 's': schema = optarg; break;
            case 'A': collect_all = 1; break;
            case 'M': metrics_enabled = 0; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    if (!validate_identifier(label)) {
        fprintf(stderr, "Error: label must match ^[A-Za-z0-9_]+$ and be <100 chars.\n");
        return 1;
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

    // A dead exporter peer must not take the daemon down with it.
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    agent_state_t state;
    memset(&state, 0, sizeof state);
    state.db = db_connect_or_die(conninfo);

    db_ensure_span_table(state.db, schema, label);
    db_prepare_span_insert(state.db, schema, label);
    if (metrics_enabled) {
        db_ensure_metric_table(state.db, schema, label);
        db_prepare_metric_insert(state.db, schema, label);
    }
    db_ensure_drop_inactive_job(state.db, schema);

    pw_http_server_t server;
    if (http_server_init(&server, bind_addr, port, on_request, &state) != 0) {
        PQfinish(state.db);
        return 1;
    }

    pw_collector_t collector;
    collector_init(&collector, !collect_all);

    printf("procwatch-agentd: OTLP/HTTP on %s:%d, schema=%s, tables=%s_spans/%s_procs\n",
           bind_addr, port, schema, label, label);
    printf("procwatch-agentd: metrics %s (interval %ds, %s)\n",
           metrics_enabled ? "enabled" : "disabled", interval,
           collect_all ? "all processes" : "injected processes only");
    fflush(stdout);

    time_t next_tick = time(NULL) + interval;

    while (!g_stop) {
        time_t now = time(NULL);
        int timeout_ms = 1000;
        if (metrics_enabled) {
            long remaining = (long)(next_tick - now) * 1000;
            timeout_ms = (remaining > 0) ? (int)remaining : 0;
            if (timeout_ms > 1000) timeout_ms = 1000; // stay responsive to signals
        }

        http_server_tick(&server, timeout_ms);

        if (metrics_enabled && time(NULL) >= next_tick) {
            // A dropped connection is the one failure worth recovering from:
            // the node keeps running while the database restarts.
            if (PQstatus(state.db) != CONNECTION_OK) {
                fprintf(stderr, "database connection lost, resetting\n");
                PQreset(state.db);
                if (PQstatus(state.db) == CONNECTION_OK) {
                    db_prepare_span_insert(state.db, schema, label);
                    db_prepare_metric_insert(state.db, schema, label);
                }
            }
            if (PQstatus(state.db) == CONNECTION_OK) {
                collector_tick(&collector, state.db);
            }
            next_tick = time(NULL) + interval;
        }
    }

    printf("procwatch-agentd: shutting down (%llu spans written, %llu dropped, "
           "%llu metric samples)\n",
           state.spans_written, state.spans_dropped, collector.samples_written);

    http_server_close(&server);
    collector_free(&collector);
    PQfinish(state.db);
    return 0;
}
