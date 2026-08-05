#pragma once

#include <postgresql/libpq-fe.h>

#include "otlp.h"

// Span, process-metric, and OTLP-metric storage for procwatch-agentd.
//
// Tables are keyed by a per-payload label (PROCWATCH_LABEL / procwatch.label).
// agentd creates <label>_spans, <label>_procs, and <label>_otel_metrics on
// first sight.

// Ensures tables exist and prepares insert statements for this label.
// Returns 0 on success.
int db_ensure_label(PGconn *conn, const char *schema, const char *label);

// Inserts using the prepared statement for span->label. Call db_ensure_label
// first (or rely on agentd to do so).
int db_insert_span(PGconn *conn, const pw_span_t *span);

int db_insert_metric_labeled(PGconn *conn, const char *schema, const char *label,
                             const char *service, const char *container_id,
                             const char *pod, const char *pid, const char *comm,
                             const char *runtime, double cpu_pct, long rss_kb,
                             long threads);

// Inserts a decoded OTLP metric data point (see otlp_decode_metrics) using
// the prepared statement for point->label. Creates <label>_otel_metrics on
// first sight of the label, same as db_ensure_label does for spans/_procs.
int db_insert_otel_metric(PGconn *conn, const pw_metric_point_t *point);

// Clears the in-process label cache (e.g. after PQreset).
void db_label_cache_reset(void);
