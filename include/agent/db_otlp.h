#pragma once

#include <postgresql/libpq-fe.h>

#include "otlp.h"

// Span and process-metric storage for procwatch-agentd.
//
// Deliberately additive: none of the functions here modify the tables or DDL
// that src/db.c creates for the procwatch binary. The span table is a
// sibling named <label>_spans, so both can share a schema and the existing
// retention and drop-inactive housekeeping.

#define PW_SPAN_STMT "pw_ins_span"
#define PW_METRIC_STMT "pw_ins_metric"

// Creates <schema>.<label>_spans as a hypertable with the same 48h retention
// and activity-tracking behaviour as the metrics table, then prepares the
// insert statement.
void db_ensure_span_table(PGconn *conn, const char *schema, const char *label);
void db_prepare_span_insert(PGconn *conn, const char *schema, const char *label);

// Returns 0 on success. Unlike the procwatch binary, agentd is long-lived and
// must not exit on a single bad row.
int db_insert_span(PGconn *conn, const pw_span_t *span);

// Process metrics table, extended beyond the procwatch binary's (ts, pid,
// cpu_pct, rss_kb) with the container and service identity the collector
// resolves.
void db_ensure_metric_table(PGconn *conn, const char *schema, const char *label);
void db_prepare_metric_insert(PGconn *conn, const char *schema, const char *label);

int db_insert_metric(PGconn *conn, const char *service, const char *container_id,
                     const char *pod, int pid, const char *comm,
                     const char *runtime, double cpu_pct, long rss_kb,
                     long threads);
