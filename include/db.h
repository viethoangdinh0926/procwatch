#pragma once

#include <postgresql/libpq-fe.h>

PGconn* db_connect_or_die(const char *conninfo);
void db_try_enable_timescaledb(PGconn *conn);
char* db_make_qualified(PGconn *conn, const char *schema, const char *table); // caller free
int db_table_exists(PGconn *conn, const char *schema, const char *table);
void db_ensure_schema(PGconn *conn, const char *schema);
void db_ensure_tracker_objects(PGconn *conn, const char *schema);
void db_register_new_table_in_tracker(PGconn *conn, const char *schema, const char *table);
void db_create_activity_triggers_for_table(PGconn *conn, const char *schema, const char *table);
void db_ensure_table(PGconn *conn, const char *schema, const char *table);
void db_prepare_insert(PGconn *conn, const char *schema, const char *table, const char *stmt_name);
void db_insert_sample(PGconn *conn, const char *stmt_name, pid_t pid, double cpu_pct, long rss_kb);
void db_ensure_pg_cron(PGconn *conn);
void db_ensure_drop_inactive_job(PGconn *conn);
