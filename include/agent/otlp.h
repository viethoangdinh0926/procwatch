#pragma once

#include <stddef.h>
#include <stdint.h>

#define PW_SPAN_NAME_MAX 512
#define PW_SPAN_SERVICE_MAX 256
#define PW_SPAN_SCOPE_MAX 256
#define PW_SPAN_STATUS_MSG_MAX 512
#define PW_SPAN_ATTRS_MAX 8192
#define PW_SPAN_LABEL_MAX 100

// One decoded OTLP span, flattened for insertion. Trace and span ids are
// kept as lowercase hex because that is how every trace UI and every hand
// written Grafana query expects to match them.
typedef struct {
    char label[PW_SPAN_LABEL_MAX];
    char trace_id[33];
    char span_id[17];
    char parent_span_id[17];
    char name[PW_SPAN_NAME_MAX];
    char service_name[PW_SPAN_SERVICE_MAX];
    char scope_name[PW_SPAN_SCOPE_MAX];
    const char *kind;
    const char *status_code;
    char status_message[PW_SPAN_STATUS_MSG_MAX];
    uint64_t start_ns;
    uint64_t end_ns;
    int64_t duration_ns;
    char attributes_json[PW_SPAN_ATTRS_MAX];
} pw_span_t;

// Invoked once per decoded span. Returning non-zero aborts the walk.
typedef int (*pw_span_sink_t)(const pw_span_t *span, void *user_data);

// Decodes an ExportTraceServiceRequest payload. Returns the number of spans
// emitted, or -1 if the payload is malformed. A truncated or partially
// corrupt payload still yields the spans decoded before the bad field.
int otlp_decode_traces(const void *data, size_t len,
                       pw_span_sink_t sink, void *user_data);

#define PW_METRIC_NAME_MAX 256
#define PW_METRIC_UNIT_MAX 64
#define PW_METRIC_DESC_MAX 256
#define PW_METRIC_TYPE_MAX 16
#define PW_METRIC_ATTRS_MAX 2048
#define PW_METRIC_PID_KEY_MAX 64

// One flattened OTLP metric data point (Gauge/Sum: one row per point;
// Histogram: one row per point holding its count+sum, buckets dropped).
typedef struct {
    char label[PW_SPAN_LABEL_MAX];
    char service_name[PW_SPAN_SERVICE_MAX];
    char scope_name[PW_SPAN_SCOPE_MAX];
    // "<pid>_<YYYYMMDDHHMMSS>", from the procwatch.pid_key resource
    // attribute the injector attaches (see pw_attach_pid_key_to_otel);
    // empty if the producer didn't set one.
    char pid_key[PW_METRIC_PID_KEY_MAX];
    char metric_name[PW_METRIC_NAME_MAX];
    char description[PW_METRIC_DESC_MAX];
    char unit[PW_METRIC_UNIT_MAX];
    const char *metric_type; // "gauge" | "sum" | "histogram"
    double value;            // gauge/sum value, or histogram sum
    int64_t count;           // histogram point count; 1 for gauge/sum
    uint64_t time_ns;
    char attributes_json[PW_METRIC_ATTRS_MAX];
} pw_metric_point_t;

// Invoked once per decoded data point. Returning non-zero aborts the walk.
typedef int (*pw_metric_sink_t)(const pw_metric_point_t *point, void *user_data);

// Decodes an ExportMetricsServiceRequest payload. Returns the number of data
// points emitted, or -1 if the payload is malformed.
int otlp_decode_metrics(const void *data, size_t len,
                        pw_metric_sink_t sink, void *user_data);
