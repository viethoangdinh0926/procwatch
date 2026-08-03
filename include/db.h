#pragma once

#include <postgresql/libpq-fe.h>

// Default housekeeping (hours). Overridable via db_set_housekeeping / CLI -R/-T.
#define PW_DEFAULT_RETENTION_HOURS  168
#define PW_DEFAULT_INACTIVE_HOURS   720

// Set before ensuring tables. Values must be > 0; invalid input keeps prior/defaults.
void db_set_housekeeping(int retention_hours, int inactive_hours);
int db_retention_hours(void);
int db_inactive_hours(void);

PGconn* db_connect_or_die(const char *conninfo);
// Non-fatal connect; returns NULL on failure.
PGconn* db_connect(const char *conninfo);
// Reset or replace a lost connection. *conn may be NULL. Returns 1 if OK.
int db_reconnect(PGconn **conn, const char *conninfo);
void db_try_enable_timescaledb(PGconn *conn);
char* db_make_qualified(PGconn *conn, const char *schema, const char *table); // caller free
int db_table_exists(PGconn *conn, const char *schema, const char *table);
void db_ensure_schema(PGconn *conn, const char *schema);
void db_ensure_tracker_objects(PGconn *conn, const char *schema);
void db_register_new_table_in_tracker(PGconn *conn, const char *schema, const char *table);
void db_create_activity_triggers_for_table(PGconn *conn, const char *schema, const char *table);
void db_ensure_table(PGconn *conn, const char *schema, const char *table);
void db_prepare_insert(PGconn *conn, const char *schema, const char *table, const char *stmt_name);
// Returns 0 on success, -1 on failure (caller may spill locally).
int db_insert_sample(PGconn *conn, const char *stmt_name, const char *pid, double cpu_pct, long rss_kb);
void db_ensure_pg_cron(PGconn *conn);
void db_ensure_drop_inactive_job(PGconn *conn, const char *schema);
