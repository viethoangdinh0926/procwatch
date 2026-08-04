// TimescaleDB storage for spans and process metrics collected by agentd.
// Tables are created on first sight of each label.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

#include "../../include/db.h"
#include "../../include/util.h"
#include "../../include/agent/db_otlp.h"

#define LABEL_CACHE_MAX 64
#define STMT_NAME_MAX 80

typedef struct {
    char label[100];
    char span_stmt[STMT_NAME_MAX];
    char metric_stmt[STMT_NAME_MAX];
    int ready;
} label_entry_t;

static label_entry_t g_labels[LABEL_CACHE_MAX];
static size_t g_label_count = 0;

void db_label_cache_reset(void) {
    memset(g_labels, 0, sizeof g_labels);
    g_label_count = 0;
}

static void exec_warn(PGconn *conn, const char *sql, const char *what) {
    PGresult *res = PQexec(conn, sql);
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        fprintf(stderr, "%s warning: %s", what, PQerrorMessage(conn));
    }
    PQclear(res);
}

static void suffixed_name(char *out, size_t cap, const char *label, const char *suffix) {
    snprintf(out, cap, "%s%s", label, suffix);
    if (strlen(out) > 63) out[63] = '\0';
}

static void hypertable_and_retention(PGconn *conn, const char *schema, const char *table) {
    char *lit_schema = PQescapeLiteral(conn, schema, strlen(schema));
    char *lit_table = PQescapeLiteral(conn, table, strlen(table));
    if (!lit_schema || !lit_table) {
        if (lit_schema) PQfreemem(lit_schema);
        if (lit_table) PQfreemem(lit_table);
        return;
    }
    char sql[1024];
    snprintf(sql, sizeof sql,
             "SELECT create_hypertable(format('%%I.%%I', %s, %s)::regclass, 'ts', if_not_exists => TRUE);",
             lit_schema, lit_table);
    exec_warn(conn, sql, "create_hypertable");
    snprintf(sql, sizeof sql,
             "SELECT remove_retention_policy(format('%%I.%%I', %s, %s)::regclass, if_exists => TRUE);",
             lit_schema, lit_table);
    exec_warn(conn, sql, "remove_retention_policy");
    snprintf(sql, sizeof sql,
             "SELECT add_retention_policy(format('%%I.%%I', %s, %s)::regclass, make_interval(hours => %d), if_not_exists => TRUE);",
             lit_schema, lit_table, db_retention_hours());
    exec_warn(conn, sql, "add_retention_policy");
    PQfreemem(lit_schema);
    PQfreemem(lit_table);
}

static void ensure_span_table(PGconn *conn, const char *schema, const char *label) {
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
    snprintf(idx, sizeof idx, "idx_%s_svc", table);
    idx[63] = '\0';
    esc_idx = PQescapeIdentifier(conn, idx, strlen(idx));
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

static void ensure_metric_table(PGconn *conn, const char *schema, const char *label) {
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
             "  pid TEXT,"
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

    // Migrate older INTEGER pid columns so "<pid>_<stamp>" keys fit.
    snprintf(sql, sizeof sql,
             "DO $$ BEGIN "
             "  ALTER TABLE %s ALTER COLUMN pid TYPE TEXT USING pid::text; "
             "EXCEPTION WHEN others THEN NULL; "
             "END $$;",
             qualified);
    exec_warn(conn, sql, "ALTER pid TYPE TEXT");

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

static void prepare_span(PGconn *conn, const char *schema, const char *label,
                         const char *stmt_name) {
    char table[80];
    suffixed_name(table, sizeof table, label, "_spans");
    char *qualified = db_make_qualified(conn, schema, table);
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO %s (ts, trace_id, span_id, parent_span_id, name, kind,"
             " service_name, scope_name, duration_ns, status_code, status_message, attributes)"
             " VALUES (to_timestamp($1::float8), $2::text, $3::text,"
             " NULLIF($4::text, ''), $5::text, $6::text, $7::text, $8::text,"
             " $9::int8, $10::text, NULLIF($11::text, ''), $12::jsonb);",
             qualified);
    PGresult *res = PQprepare(conn, stmt_name, sql, 12, NULL);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
        fprintf(stderr, "PQprepare span insert failed: %s\n", PQerrorMessage(conn));
    PQclear(res);
    free(qualified);
}

static void prepare_metric(PGconn *conn, const char *schema, const char *label,
                           const char *stmt_name) {
    char table[80];
    suffixed_name(table, sizeof table, label, "_procs");
    char *qualified = db_make_qualified(conn, schema, table);
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO %s (ts, service_name, container_id, pod, pid, comm,"
             " runtime, cpu_pct, rss_kb, threads)"
             " VALUES (NOW(), $1::text, NULLIF($2::text, ''), NULLIF($3::text, ''),"
             " $4::text, $5::text, $6::text, $7::float8, $8::int8, $9::int4);",
             qualified);
    PGresult *res = PQprepare(conn, stmt_name, sql, 9, NULL);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
        fprintf(stderr, "PQprepare metric insert failed: %s\n", PQerrorMessage(conn));
    PQclear(res);
    free(qualified);
}

static label_entry_t *find_label(const char *label) {
    for (size_t i = 0; i < g_label_count; ++i) {
        if (strcmp(g_labels[i].label, label) == 0) return &g_labels[i];
    }
    return NULL;
}

int db_ensure_label(PGconn *conn, const char *schema, const char *label) {
    if (!conn || PQstatus(conn) != CONNECTION_OK) return -1;
    if (!validate_identifier(label) || !validate_identifier(schema)) return -1;

    label_entry_t *e = find_label(label);
    if (e && e->ready) return 0;

    if (!e) {
        if (g_label_count >= LABEL_CACHE_MAX) {
            fprintf(stderr, "label cache full, cannot add %s\n", label);
            return -1;
        }
        e = &g_labels[g_label_count++];
        memset(e, 0, sizeof *e);
        snprintf(e->label, sizeof e->label, "%s", label);
        // Prepared statement names must be unique and identifier-safe.
        snprintf(e->span_stmt, sizeof e->span_stmt, "pw_span_%s", label);
        snprintf(e->metric_stmt, sizeof e->metric_stmt, "pw_metric_%s", label);
    }

    ensure_span_table(conn, schema, label);
    ensure_metric_table(conn, schema, label);
    prepare_span(conn, schema, label, e->span_stmt);
    prepare_metric(conn, schema, label, e->metric_stmt);
    e->ready = 1;
    return 0;
}

int db_insert_span(PGconn *conn, const pw_span_t *span) {
    if (!conn || PQstatus(conn) != CONNECTION_OK) return -1;
    label_entry_t *e = find_label(span->label);
    if (!e || !e->ready) {
        fprintf(stderr, "span insert: label %s not prepared\n", span->label);
        return -1;
    }

    char ts_buf[64], dur_buf[32];
    if (span->end_ns == 0)
        snprintf(ts_buf, sizeof ts_buf, "%.6f", (double)span->start_ns / 1e9);
    else
        snprintf(ts_buf, sizeof ts_buf, "%.6f", (double)span->end_ns / 1e9);
    snprintf(dur_buf, sizeof dur_buf, "%lld", (long long)span->duration_ns);

    const char *vals[12] = {
        ts_buf, span->trace_id, span->span_id, span->parent_span_id,
        span->name, span->kind, span->service_name, span->scope_name,
        dur_buf, span->status_code, span->status_message, span->attributes_json,
    };
    const int lens[12] = {0};
    const int fmts[12] = {0};

    PGresult *res = PQexecPrepared(conn, e->span_stmt, 12, vals, lens, fmts, 0);
    int ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    if (!ok) fprintf(stderr, "span insert failed: %s", PQerrorMessage(conn));
    PQclear(res);
    return ok ? 0 : -1;
}

int db_insert_metric_labeled(PGconn *conn, const char *schema, const char *label,
                             const char *service, const char *container_id,
                             const char *pod, const char *pid, const char *comm,
                             const char *runtime, double cpu_pct, long rss_kb,
                             long threads) {
    (void)schema;
    if (!conn || PQstatus(conn) != CONNECTION_OK) return -1;
    label_entry_t *e = find_label(label);
    if (!e || !e->ready) {
        fprintf(stderr, "metric insert: label %s not prepared\n", label);
        return -1;
    }

    char cpu_buf[64], rss_buf[32], thr_buf[16];
    snprintf(cpu_buf, sizeof cpu_buf, "%.10g", cpu_pct);
    snprintf(rss_buf, sizeof rss_buf, "%ld", rss_kb);
    snprintf(thr_buf, sizeof thr_buf, "%ld", threads);

    const char *vals[9] = {
        service, container_id ? container_id : "", pod ? pod : "",
        pid ? pid : "", comm, runtime, cpu_buf, rss_buf, thr_buf
    };
    const int lens[9] = {0};
    const int fmts[9] = {0};

    PGresult *res = PQexecPrepared(conn, e->metric_stmt, 9, vals, lens, fmts, 0);
    int ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    if (!ok) fprintf(stderr, "metric insert failed: %s", PQerrorMessage(conn));
    PQclear(res);
    return ok ? 0 : -1;
}
