// TimescaleDB storage for spans and process metrics collected by agentd.
//
// This reuses the helpers in src/db.c (db_make_qualified, db_ensure_schema,
// the tracker objects) but creates its own tables. Nothing here alters the
// DDL path the procwatch binary depends on.
//
// Error handling differs from src/db.c on purpose: the procwatch binary is a
// short-lived foreground tool and exits on a bad statement, whereas agentd is
// a long-running daemon where one malformed span must not take down
// collection for the whole node.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

#include "../../include/db.h"
#include "../../include/agent/db_otlp.h"

static void exec_warn(PGconn *conn, const char *sql, const char *what) {
    PGresult *res = PQexec(conn, sql);
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        fprintf(stderr, "%s warning: %s", what, PQerrorMessage(conn));
    }
    PQclear(res);
}

// Builds "<label>_spans" / "<label>_metrics". The label is validated by the
// caller against ^[A-Za-z0-9_]+$, and identifiers are escaped downstream, so
// the concatenation is safe.
static void suffixed_name(char *out, size_t cap, const char *label, const char *suffix) {
    snprintf(out, cap, "%s%s", label, suffix);
    // Postgres truncates identifiers at 63 bytes; do it ourselves so the name
    // we register in the tracker matches the name Postgres actually created.
    if (strlen(out) > 63) out[63] = '\0';
}

static void hypertable_and_retention(PGconn *conn, const char *schema, const char *table) {
    char *lit_schema = PQescapeLiteral(conn, schema, strlen(schema));
    char *lit_table = PQescapeLiteral(conn, table, strlen(table));
    if (!lit_schema || !lit_table) {
        if (lit_schema) PQfreemem(lit_schema);
        if (lit_table) PQfreemem(lit_table);
        fprintf(stderr, "Failed to escape literals for %s\n", table);
        return;
    }

    char sql[1024];
    snprintf(sql, sizeof sql,
             "SELECT create_hypertable(format('%%I.%%I', %s, %s)::regclass, 'ts', if_not_exists => TRUE);",
             lit_schema, lit_table);
    exec_warn(conn, sql, "create_hypertable");

    snprintf(sql, sizeof sql,
             "SELECT add_retention_policy(format('%%I.%%I', %s, %s)::regclass, INTERVAL '48 hours', if_not_exists => TRUE);",
             lit_schema, lit_table);
    exec_warn(conn, sql, "add_retention_policy");

    PQfreemem(lit_schema);
    PQfreemem(lit_table);
}

// ------------------------ Spans ------------------------

void db_ensure_span_table(PGconn *conn, const char *schema, const char *label) {
    char table[80];
    suffixed_name(table, sizeof table, label, "_spans");

    int existed_before = db_table_exists(conn, schema, table);

    db_ensure_schema(conn, schema);
    db_try_enable_timescaledb(conn);

    char *qualified = db_make_qualified(conn, schema, table);
    char sql[2048];
    snprintf(sql, sizeof sql,
             "CREATE TABLE IF NOT EXISTS %s ("
             "  ts TIMESTAMPTZ NOT NULL,"
             "  trace_id TEXT NOT NULL,"
             "  span_id TEXT NOT NULL,"
             "  parent_span_id TEXT,"
             "  name TEXT,"
             "  kind TEXT,"
             "  service_name TEXT,"
             "  scope_name TEXT,"
             "  duration_ns BIGINT,"
             "  status_code TEXT,"
             "  status_message TEXT,"
             "  attributes JSONB"
             ");",
             qualified);
    PGresult *res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "CREATE span table failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        free(qualified);
        return;
    }
    PQclear(res);

    // Trace lookup is the dominant query shape in Grafana, and the hypertable
    // only indexes ts by default.
    char idx[80];
    snprintf(idx, sizeof idx, "idx_%s_trace", table);
    idx[63] = '\0';
    char *esc_idx = PQescapeIdentifier(conn, idx, strlen(idx));
    if (esc_idx) {
        snprintf(sql, sizeof sql,
                 "CREATE INDEX IF NOT EXISTS %s ON %s (trace_id, ts DESC);",
                 esc_idx, qualified);
        exec_warn(conn, sql, "CREATE INDEX");
        PQfreemem(esc_idx);
    }

    char idx2[80];
    snprintf(idx2, sizeof idx2, "idx_%s_svc", table);
    idx2[63] = '\0';
    char *esc_idx2 = PQescapeIdentifier(conn, idx2, strlen(idx2));
    if (esc_idx2) {
        snprintf(sql, sizeof sql,
                 "CREATE INDEX IF NOT EXISTS %s ON %s (service_name, ts DESC);",
                 esc_idx2, qualified);
        exec_warn(conn, sql, "CREATE INDEX");
        PQfreemem(esc_idx2);
    }

    free(qualified);
    hypertable_and_retention(conn, schema, table);

    if (!existed_before && db_table_exists(conn, schema, table)) {
        db_ensure_tracker_objects(conn, schema);
        db_register_new_table_in_tracker(conn, schema, table);
        db_create_activity_triggers_for_table(conn, schema, table);
    }
}

void db_prepare_span_insert(PGconn *conn, const char *schema, const char *label) {
    char table[80];
    suffixed_name(table, sizeof table, label, "_spans");
    char *qualified = db_make_qualified(conn, schema, table);

    char sql[1024];
    // ts comes from the span's own end timestamp rather than NOW() so that
    // batches delayed by the exporter still land in the right time bucket.
    snprintf(sql, sizeof sql,
             "INSERT INTO %s (ts, trace_id, span_id, parent_span_id, name, kind,"
             " service_name, scope_name, duration_ns, status_code, status_message, attributes)"
             " VALUES (to_timestamp($1::float8), $2::text, $3::text,"
             " NULLIF($4::text, ''), $5::text, $6::text, $7::text, $8::text,"
             " $9::int8, $10::text, NULLIF($11::text, ''), $12::jsonb);",
             qualified);

    PGresult *res = PQprepare(conn, PW_SPAN_STMT, sql, 12, NULL);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "PQprepare span insert failed: %s\n", PQerrorMessage(conn));
    }
    PQclear(res);
    free(qualified);
}

int db_insert_span(PGconn *conn, const pw_span_t *span) {
    char ts_buf[64], dur_buf[32];

    // Nanoseconds since epoch as a float8 for to_timestamp(); doubles hold
    // microsecond resolution at current epoch values, which matches the
    // resolution TIMESTAMPTZ stores anyway.
    double end_seconds = (double)span->end_ns / 1e9;
    if (span->end_ns == 0) {
        snprintf(ts_buf, sizeof ts_buf, "%.6f", (double)span->start_ns / 1e9);
    } else {
        snprintf(ts_buf, sizeof ts_buf, "%.6f", end_seconds);
    }
    snprintf(dur_buf, sizeof dur_buf, "%lld", (long long)span->duration_ns);

    const char *vals[12] = {
        ts_buf,
        span->trace_id,
        span->span_id,
        span->parent_span_id,
        span->name,
        span->kind,
        span->service_name,
        span->scope_name,
        dur_buf,
        span->status_code,
        span->status_message,
        span->attributes_json,
    };
    const int lens[12] = {0};
    const int fmts[12] = {0};

    PGresult *res = PQexecPrepared(conn, PW_SPAN_STMT, 12, vals, lens, fmts, 0);
    int ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    if (!ok) fprintf(stderr, "span insert failed: %s", PQerrorMessage(conn));
    PQclear(res);
    return ok ? 0 : -1;
}

// ------------------------ Process metrics ------------------------

void db_ensure_metric_table(PGconn *conn, const char *schema, const char *label) {
    char table[80];
    suffixed_name(table, sizeof table, label, "_procs");

    int existed_before = db_table_exists(conn, schema, table);

    db_ensure_schema(conn, schema);
    db_try_enable_timescaledb(conn);

    char *qualified = db_make_qualified(conn, schema, table);
    char sql[2048];
    snprintf(sql, sizeof sql,
             "CREATE TABLE IF NOT EXISTS %s ("
             "  ts TIMESTAMPTZ NOT NULL,"
             "  service_name TEXT,"
             "  container_id TEXT,"
             "  pod TEXT,"
             "  pid INTEGER,"
             "  comm TEXT,"
             "  runtime TEXT,"
             "  cpu_pct DOUBLE PRECISION,"
             "  rss_kb BIGINT,"
             "  threads INTEGER"
             ");",
             qualified);
    PGresult *res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "CREATE metric table failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        free(qualified);
        return;
    }
    PQclear(res);

    char idx[80];
    snprintf(idx, sizeof idx, "idx_%s_svc", table);
    idx[63] = '\0';
    char *esc_idx = PQescapeIdentifier(conn, idx, strlen(idx));
    if (esc_idx) {
        snprintf(sql, sizeof sql,
                 "CREATE INDEX IF NOT EXISTS %s ON %s (service_name, ts DESC);",
                 esc_idx, qualified);
        exec_warn(conn, sql, "CREATE INDEX");
        PQfreemem(esc_idx);
    }

    free(qualified);
    hypertable_and_retention(conn, schema, table);

    if (!existed_before && db_table_exists(conn, schema, table)) {
        db_ensure_tracker_objects(conn, schema);
        db_register_new_table_in_tracker(conn, schema, table);
        db_create_activity_triggers_for_table(conn, schema, table);
    }
}

void db_prepare_metric_insert(PGconn *conn, const char *schema, const char *label) {
    char table[80];
    suffixed_name(table, sizeof table, label, "_procs");
    char *qualified = db_make_qualified(conn, schema, table);

    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO %s (ts, service_name, container_id, pod, pid, comm,"
             " runtime, cpu_pct, rss_kb, threads)"
             " VALUES (NOW(), $1::text, NULLIF($2::text, ''), NULLIF($3::text, ''),"
             " $4::int4, $5::text, $6::text, $7::float8, $8::int8, $9::int4);",
             qualified);

    PGresult *res = PQprepare(conn, PW_METRIC_STMT, sql, 9, NULL);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "PQprepare metric insert failed: %s\n", PQerrorMessage(conn));
    }
    PQclear(res);
    free(qualified);
}

int db_insert_metric(PGconn *conn, const char *service, const char *container_id,
                     const char *pod, int pid, const char *comm,
                     const char *runtime, double cpu_pct, long rss_kb,
                     long threads) {
    char pid_buf[16], cpu_buf[64], rss_buf[32], thr_buf[16];
    snprintf(pid_buf, sizeof pid_buf, "%d", pid);
    snprintf(cpu_buf, sizeof cpu_buf, "%.10g", cpu_pct);
    snprintf(rss_buf, sizeof rss_buf, "%ld", rss_kb);
    snprintf(thr_buf, sizeof thr_buf, "%ld", threads);

    const char *vals[9] = {
        service, container_id, pod, pid_buf, comm, runtime, cpu_buf, rss_buf, thr_buf
    };
    const int lens[9] = {0};
    const int fmts[9] = {0};

    PGresult *res = PQexecPrepared(conn, PW_METRIC_STMT, 9, vals, lens, fmts, 0);
    int ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    if (!ok) fprintf(stderr, "metric insert failed: %s", PQerrorMessage(conn));
    PQclear(res);
    return ok ? 0 : -1;
}
