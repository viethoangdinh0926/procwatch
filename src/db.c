#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

#include "../include/util.h"
#include "../include/db.h"

PGconn* db_connect_or_die(const char *conninfo) {
    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "PostgreSQL connection failed: %s\n", PQerrorMessage(conn));
        exit(EXIT_FAILURE);
    }
    return conn;
}

void db_try_enable_timescaledb(PGconn *conn) {
    PGresult *res = PQexec(conn, "CREATE EXTENSION IF NOT EXISTS timescaledb;");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Warning: could not ensure timescaledb extension: %s",
                PQerrorMessage(conn));
    }
    PQclear(res);
}

char* db_make_qualified(PGconn *conn, const char *schema, const char *table) {
    char *esc_schema = PQescapeIdentifier(conn, schema, strlen(schema));
    char *esc_table  = PQescapeIdentifier(conn, table,  strlen(table));
    if (!esc_schema || !esc_table) {
        if (esc_schema) PQfreemem(esc_schema);
        if (esc_table)  PQfreemem(esc_table);
        fprintf(stderr, "Failed to escape identifiers\n");
        exit(EXIT_FAILURE);
    }
    size_t len = strlen(esc_schema) + 1 + strlen(esc_table) + 1;
    char *q = (char*)malloc(len);
    if (!q) { fprintf(stderr, "OOM\n"); exit(EXIT_FAILURE); }
    snprintf(q, len, "%s.%s", esc_schema, esc_table);
    PQfreemem(esc_schema);
    PQfreemem(esc_table);
    return q; // caller free()
}

int db_table_exists(PGconn *conn, const char *schema, const char *table) {
    char *lit_schema = PQescapeLiteral(conn, schema, strlen(schema));
    char *lit_table  = PQescapeLiteral(conn, table,  strlen(table));
    if (!lit_schema || !lit_table) {
        if (lit_schema) PQfreemem(lit_schema);
        if (lit_table)  PQfreemem(lit_table);
        fprintf(stderr, "Failed to allocate literals for existence check\n");
        exit(EXIT_FAILURE);
    }
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT to_regclass(format('%%I.%%I', %s, %s));",
             lit_schema, lit_table);
    PQfreemem(lit_schema);
    PQfreemem(lit_table);

    PGresult *res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Existence check failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        exit(EXIT_FAILURE);
    }
    int exists = 0;
    if (PQntuples(res) == 1 && !PQgetisnull(res, 0, 0)) {
        const char *val = PQgetvalue(res, 0, 0);
        exists = (val && *val) ? 1 : 0;
    }
    PQclear(res);
    return exists;
}

void db_ensure_schema(PGconn *conn, const char *schema) {
    char *esc_schema = PQescapeIdentifier(conn, schema, strlen(schema));
    if (!esc_schema) { fprintf(stderr, "Failed to escape schema\n"); exit(EXIT_FAILURE); }

    char sql[512];

    snprintf(sql, sizeof(sql), "CREATE SCHEMA IF NOT EXISTS %s AUTHORIZATION CURRENT_USER;", esc_schema);
    PGresult *res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "CREATE SCHEMA failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        PQfreemem(esc_schema);
        exit(EXIT_FAILURE);
    }
    PQclear(res);

    snprintf(sql, sizeof(sql), "GRANT USAGE ON SCHEMA %s TO CURRENT_USER;", esc_schema);
    res = PQexec(conn, sql);
    PQclear(res);

    PQfreemem(esc_schema);
}

void db_ensure_tracker_objects(PGconn *conn, const char *schema) {
    char *qualified_tracker = db_make_qualified(conn, schema, "table_activity_tracker");
    char *esc_schema = PQescapeIdentifier(conn, schema, strlen(schema));
    if (!esc_schema) { fprintf(stderr, "Failed to escape schema\n"); exit(EXIT_FAILURE); }

    char sql[2048];
    PGresult *res;

    snprintf(sql, sizeof(sql),
             "CREATE TABLE IF NOT EXISTS %s ("
             "  schema_name  TEXT NOT NULL,"
             "  table_name   TEXT NOT NULL,"
             "  last_write   TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
             "  PRIMARY KEY(schema_name, table_name)"
             ");",
             qualified_tracker);
    res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "CREATE tracker table failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        free(qualified_tracker);
        PQfreemem(esc_schema);
        exit(EXIT_FAILURE);
    }
    PQclear(res);

    snprintf(sql, sizeof(sql),
             "CREATE OR REPLACE FUNCTION %s.update_last_write()\n"
             "RETURNS TRIGGER AS $$\n"
             "BEGIN\n"
             "  UPDATE %s\n"
             "     SET last_write = NOW()\n"
             "   WHERE schema_name = TG_TABLE_SCHEMA\n"
             "     AND table_name  = TG_TABLE_NAME;\n"
             "  RETURN NEW;\n"
             "END;\n"
             "$$ LANGUAGE plpgsql;",
             esc_schema, qualified_tracker);
    res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "CREATE FUNCTION update_last_write failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        free(qualified_tracker);
        PQfreemem(esc_schema);
        exit(EXIT_FAILURE);
    }
    PQclear(res);

    PQfreemem(esc_schema);
    free(qualified_tracker);
}

void db_register_new_table_in_tracker(PGconn *conn, const char *schema, const char *table) {
    char *qualified_tracker = db_make_qualified(conn, schema, "table_activity_tracker");
    char *lit_schema = PQescapeLiteral(conn, schema, strlen(schema));
    char *lit_table  = PQescapeLiteral(conn, table,  strlen(table));
    if (!lit_schema || !lit_table) {
        if (lit_schema) PQfreemem(lit_schema);
        if (lit_table)  PQfreemem(lit_table);
        free(qualified_tracker);
        fprintf(stderr, "Failed to allocate literals for tracker insert\n");
        exit(EXIT_FAILURE);
    }

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO %s (schema_name, table_name, last_write) "
             "VALUES (%s, %s, NOW()) "
             "ON CONFLICT (schema_name, table_name) DO NOTHING;",
             qualified_tracker, lit_schema, lit_table);
    PQfreemem(lit_schema);
    PQfreemem(lit_table);

    PGresult *res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Tracker INSERT failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        free(qualified_tracker);
        exit(EXIT_FAILURE);
    }
    PQclear(res);
    free(qualified_tracker);
}

void db_create_activity_triggers_for_table(PGconn *conn, const char *schema, const char *table) {
    char *qualified = db_make_qualified(conn, schema, table);

    char trg_ins[80], trg_upd[80], trg_del[80];
    snprintf(trg_ins, sizeof(trg_ins), "trg_ulw_ins_%s", table);
    snprintf(trg_upd, sizeof(trg_upd), "trg_ulw_upd_%s", table);
    snprintf(trg_del, sizeof(trg_del), "trg_ulw_del_%s", table);

    trg_ins[63] = '\0'; trg_upd[63] = '\0'; trg_del[63] = '\0';

    char *esc_trg_ins = PQescapeIdentifier(conn, trg_ins, strlen(trg_ins));
    char *esc_trg_upd = PQescapeIdentifier(conn, trg_upd, strlen(trg_upd));
    char *esc_trg_del = PQescapeIdentifier(conn, trg_del, strlen(trg_del));
    char *esc_schema  = PQescapeIdentifier(conn, schema, strlen(schema));

    if (!esc_trg_ins || !esc_trg_upd || !esc_trg_del || !esc_schema) {
        if (esc_trg_ins) PQfreemem(esc_trg_ins);
        if (esc_trg_upd) PQfreemem(esc_trg_upd);
        if (esc_trg_del) PQfreemem(esc_trg_del);
        if (esc_schema)  PQfreemem(esc_schema);
        free(qualified);
        fprintf(stderr, "Failed to escape trigger identifiers\n");
        exit(EXIT_FAILURE);
    }

    char sql[1024];
    PGresult *res;

    snprintf(sql, sizeof(sql),
             "CREATE TRIGGER %s AFTER INSERT ON %s "
             "FOR EACH STATEMENT EXECUTE FUNCTION %s.update_last_write();",
             esc_trg_ins, qualified, esc_schema);
    res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "CREATE TRIGGER (INSERT) failed: %s\n", PQerrorMessage(conn));
    }
    PQclear(res);

    snprintf(sql, sizeof(sql),
             "CREATE TRIGGER %s AFTER UPDATE ON %s "
             "FOR EACH STATEMENT EXECUTE FUNCTION %s.update_last_write();",
             esc_trg_upd, qualified, esc_schema);
    res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "CREATE TRIGGER (UPDATE) failed: %s\n", PQerrorMessage(conn));
    }
    PQclear(res);

    snprintf(sql, sizeof(sql),
             "CREATE TRIGGER %s AFTER DELETE ON %s "
             "FOR EACH STATEMENT EXECUTE FUNCTION %s.update_last_write();",
             esc_trg_del, qualified, esc_schema);
    res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "CREATE TRIGGER (DELETE) failed: %s\n", PQerrorMessage(conn));
    }
    PQclear(res);

    PQfreemem(esc_trg_ins);
    PQfreemem(esc_trg_upd);
    PQfreemem(esc_trg_del);
    PQfreemem(esc_schema);
    free(qualified);
}

void db_ensure_table(PGconn *conn, const char *schema, const char *table) {
    int existed_before = db_table_exists(conn, schema, table);

    db_ensure_schema(conn, schema);
    db_try_enable_timescaledb(conn);

    char *qualified = db_make_qualified(conn, schema, table);
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "CREATE TABLE IF NOT EXISTS %s ("
             "  ts TIMESTAMPTZ NOT NULL,"
             "  pid TEXT NOT NULL,"
             "  cpu_pct DOUBLE PRECISION,"
             "  rss_kb BIGINT"
             ");",
             qualified);
    PGresult *res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "CREATE TABLE failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        free(qualified);
        exit(EXIT_FAILURE);
    }
    PQclear(res);
    free(qualified);

    char *lit_schema = PQescapeLiteral(conn, schema, strlen(schema));
    char *lit_table  = PQescapeLiteral(conn, table,  strlen(table));
    if (!lit_schema || !lit_table) {
        if (lit_schema) PQfreemem(lit_schema);
        if (lit_table)  PQfreemem(lit_table);
        fprintf(stderr, "Failed to allocate literals\n");
        exit(EXIT_FAILURE);
    }
    snprintf(sql, sizeof(sql),
             "SELECT create_hypertable(format('%%I.%%I', %s, %s)::regclass, 'ts', if_not_exists => TRUE);",
             lit_schema, lit_table);
    res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_TUPLES_OK &&
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "create_hypertable warning: %s\n", PQerrorMessage(conn));
    }
    PQclear(res);

    snprintf(sql, sizeof(sql),
             "SELECT add_retention_policy(format('%%I.%%I', %s, %s)::regclass, INTERVAL '48 hours', if_not_exists => TRUE);",
             lit_schema, lit_table);
    res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_TUPLES_OK &&
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "add_retention_policy warning: %s\n", PQerrorMessage(conn));
    }
    PQclear(res);

    int existed_after = db_table_exists(conn, schema, table);

    if (!existed_before && existed_after) {
        db_ensure_tracker_objects(conn, schema);
        db_register_new_table_in_tracker(conn, schema, table);
        db_create_activity_triggers_for_table(conn, schema, table);
    }

    PQfreemem(lit_schema);
    PQfreemem(lit_table);
}

void db_prepare_insert(PGconn *conn, const char *schema, const char *table, const char *stmt_name) {
    char *qualified = db_make_qualified(conn, schema, table);

    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO %s (ts, pid, cpu_pct, rss_kb) VALUES (NOW(), $1::text, $2::float8, $3::int8);",
             qualified);

    PGresult *res = PQprepare(conn, stmt_name, sql, 3, NULL);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "PQprepare failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        free(qualified);
        exit(EXIT_FAILURE);
    }
    PQclear(res);
    free(qualified);
}

void db_insert_sample(PGconn *conn, const char *stmt_name,
                      pid_t pid, double cpu_pct, long rss_kb)
{
    char pid_buf[32], cpu_buf[64], rss_buf[64];
    snprintf(pid_buf, sizeof(pid_buf), "%d", (int)pid);
    snprintf(cpu_buf, sizeof(cpu_buf), "%.10g", cpu_pct);
    snprintf(rss_buf, sizeof(rss_buf), "%ld", rss_kb);

    const char *vals[3] = { pid_buf, cpu_buf, rss_buf };
    const int   lens[3] = { 0, 0, 0 };
    const int   fmts[3] = { 0, 0, 0 };

    PGresult *res = PQexecPrepared(conn, stmt_name, 3, vals, lens, fmts, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Insert failed: %s\n", PQerrorMessage(conn));
    }
    PQclear(res);
}


void db_ensure_drop_inactive_job(PGconn *conn) {
    // Check if job exists
    PGresult *res = PQexec(conn,
        "SELECT jobid FROM cron.job WHERE jobname = 'drop_inactive_tables_every_5_minutes';");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "cron job lookup failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        return;
    }

    int exists = PQntuples(res) > 0;
    PQclear(res);

    if (!exists) {
        res = PQexec(conn,
            "SELECT cron.schedule("
            "  'drop_inactive_tables_every_5_minutes',"
            "  '*/5 * * * *',"
            "  $$SELECT procwatch.drop_inactive_tables();$$"
            ");");
        if (PQresultStatus(res) != PGRES_TUPLES_OK && PQresultStatus(res) != PGRES_COMMAND_OK) {
            fprintf(stderr, "cron.schedule failed: %s\n", PQerrorMessage(conn));
            PQclear(res);
            return;
        }
        PQclear(res);
    }

    // Ensure nodename is NULL/empty to force Unix socket
    res = PQexec(conn,
        "UPDATE cron.job SET nodename = '' WHERE jobname = 'drop_inactive_tables_every_5_minutes';");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Failed to update cron job nodename: %s\n", PQerrorMessage(conn));
    }
    PQclear(res);
}
