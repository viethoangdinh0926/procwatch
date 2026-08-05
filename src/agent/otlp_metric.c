// Decodes OTLP ExportMetricsServiceRequest payloads into flat data-point
// records, one row per (metric, data point). Gauge and Sum points carry a
// single numeric value; Histogram points are flattened to their count+sum,
// with individual buckets dropped (the schema this feeds is row-per-point,
// not row-per-bucket).
//
// Field numbers are taken from opentelemetry-proto's metrics.proto.
// KeyValue/AnyValue attribute decoding is shared with otlp_trace.c via
// otlp_common.h.

#include <stdio.h>
#include <string.h>

#include "../../include/agent/pb_decode.h"
#include "../../include/agent/otlp.h"
#include "../../include/agent/otlp_common.h"

// ExportMetricsServiceRequest
#define F_EMSR_RESOURCE_METRICS 1

// ResourceMetrics
#define F_RM_RESOURCE 1
#define F_RM_SCOPE_METRICS 2

// Resource
#define F_RES_ATTRIBUTES 1

// ScopeMetrics
#define F_SM_SCOPE 1
#define F_SM_METRICS 2

// InstrumentationScope
#define F_IS_NAME 1

// Metric
#define F_METRIC_NAME 1
#define F_METRIC_DESCRIPTION 2
#define F_METRIC_UNIT 3
#define F_METRIC_GAUGE 5
#define F_METRIC_SUM 7
#define F_METRIC_HISTOGRAM 9

// Gauge / Sum
#define F_GAUGE_DATA_POINTS 1
#define F_SUM_DATA_POINTS 1

// Histogram
#define F_HIST_DATA_POINTS 1

// NumberDataPoint
#define F_NDP_START_TIME 2
#define F_NDP_TIME 3
#define F_NDP_AS_DOUBLE 4
#define F_NDP_EXEMPLARS 5
#define F_NDP_AS_INT 6
#define F_NDP_ATTRIBUTES 7

// HistogramDataPoint
#define F_HDP_ATTRIBUTES 9
#define F_HDP_START_TIME 2
#define F_HDP_TIME 3
#define F_HDP_COUNT 4
#define F_HDP_SUM 5

typedef struct {
    char service_name[PW_SPAN_SERVICE_MAX];
    char scope_name[PW_SPAN_SCOPE_MAX];
    char label[PW_SPAN_LABEL_MAX];
    char pid_key[PW_METRIC_PID_KEY_MAX];
} metric_context_t;

// Shared by NumberDataPoint and HistogramDataPoint: both put their attributes
// in a repeated KeyValue field and are otherwise walked field-by-field.
static void decode_point_attrs(pb_reader_t *kv, pw_sb_t *attrs, int *first,
                               int *have_attrs) {
    size_t mark = attrs->len;
    if (!*first) pw_sb_putc(attrs, ',');
    if (pw_append_kv_json(kv, attrs, 0)) {
        *first = 0;
        *have_attrs = 1;
    } else {
        attrs->len = mark;
        attrs->buf[mark] = '\0';
    }
}

static int decode_number_point(pb_reader_t *r, const metric_context_t *ctx,
                               const char *name, const char *description,
                               const char *unit, const char *metric_type,
                               pw_metric_sink_t sink, void *user_data) {
    pw_metric_point_t pt;
    memset(&pt, 0, sizeof pt);
    snprintf(pt.label, sizeof pt.label, "%s", ctx->label);
    snprintf(pt.service_name, sizeof pt.service_name, "%s", ctx->service_name);
    snprintf(pt.scope_name, sizeof pt.scope_name, "%s", ctx->scope_name);
    snprintf(pt.pid_key, sizeof pt.pid_key, "%s", ctx->pid_key);
    snprintf(pt.metric_name, sizeof pt.metric_name, "%s", name);
    snprintf(pt.description, sizeof pt.description, "%s", description);
    snprintf(pt.unit, sizeof pt.unit, "%s", unit);
    pt.metric_type = metric_type;
    pt.count = 1;

    pw_sb_t attrs;
    pw_sb_init(&attrs, pt.attributes_json, sizeof pt.attributes_json);
    pw_sb_putc(&attrs, '{');
    int first_attr = 1, have_attrs = 0;

    pb_tag_t tag;
    while (pb_next(r, &tag) == 1) {
        switch (tag.field) {
            case F_NDP_TIME:
                if (tag.wire == PB_WIRE_FIXED64) {
                    if (pb_fixed64(r, &pt.time_ns) != 1) return -1;
                    continue;
                }
                break;
            case F_NDP_AS_DOUBLE:
                if (tag.wire == PB_WIRE_FIXED64) {
                    uint64_t bits;
                    if (pb_fixed64(r, &bits) != 1) return -1;
                    pt.value = pb_bits_to_double(bits);
                    continue;
                }
                break;
            case F_NDP_AS_INT:
                if (tag.wire == PB_WIRE_FIXED64) {
                    uint64_t bits;
                    if (pb_fixed64(r, &bits) != 1) return -1;
                    pt.value = (double)(int64_t)bits;
                    continue;
                }
                break;
            case F_NDP_ATTRIBUTES:
                if (tag.wire == PB_WIRE_BYTES) {
                    pb_reader_t kv;
                    if (pb_submsg(r, &kv) != 1) return -1;
                    decode_point_attrs(&kv, &attrs, &first_attr, &have_attrs);
                    continue;
                }
                break;
            default:
                break;
        }
        if (pb_skip(r, tag.wire) != 1) return -1;
    }

    pw_sb_putc(&attrs, '}');
    if (!have_attrs) snprintf(pt.attributes_json, sizeof pt.attributes_json, "{}");

    return sink ? sink(&pt, user_data) : 0;
}

static int decode_histogram_point(pb_reader_t *r, const metric_context_t *ctx,
                                  const char *name, const char *description,
                                  const char *unit,
                                  pw_metric_sink_t sink, void *user_data) {
    pw_metric_point_t pt;
    memset(&pt, 0, sizeof pt);
    snprintf(pt.label, sizeof pt.label, "%s", ctx->label);
    snprintf(pt.service_name, sizeof pt.service_name, "%s", ctx->service_name);
    snprintf(pt.scope_name, sizeof pt.scope_name, "%s", ctx->scope_name);
    snprintf(pt.pid_key, sizeof pt.pid_key, "%s", ctx->pid_key);
    snprintf(pt.metric_name, sizeof pt.metric_name, "%s", name);
    snprintf(pt.description, sizeof pt.description, "%s", description);
    snprintf(pt.unit, sizeof pt.unit, "%s", unit);
    pt.metric_type = "histogram";

    pw_sb_t attrs;
    pw_sb_init(&attrs, pt.attributes_json, sizeof pt.attributes_json);
    pw_sb_putc(&attrs, '{');
    int first_attr = 1, have_attrs = 0;

    pb_tag_t tag;
    while (pb_next(r, &tag) == 1) {
        switch (tag.field) {
            case F_HDP_TIME:
                if (tag.wire == PB_WIRE_FIXED64) {
                    if (pb_fixed64(r, &pt.time_ns) != 1) return -1;
                    continue;
                }
                break;
            case F_HDP_COUNT:
                if (tag.wire == PB_WIRE_FIXED64) {
                    uint64_t v;
                    if (pb_fixed64(r, &v) != 1) return -1;
                    pt.count = (int64_t)v;
                    continue;
                }
                break;
            case F_HDP_SUM:
                if (tag.wire == PB_WIRE_FIXED64) {
                    uint64_t bits;
                    if (pb_fixed64(r, &bits) != 1) return -1;
                    pt.value = pb_bits_to_double(bits);
                    continue;
                }
                break;
            case F_HDP_ATTRIBUTES:
                if (tag.wire == PB_WIRE_BYTES) {
                    pb_reader_t kv;
                    if (pb_submsg(r, &kv) != 1) return -1;
                    decode_point_attrs(&kv, &attrs, &first_attr, &have_attrs);
                    continue;
                }
                break;
            default:
                break;
        }
        if (pb_skip(r, tag.wire) != 1) return -1;
    }

    pw_sb_putc(&attrs, '}');
    if (!have_attrs) snprintf(pt.attributes_json, sizeof pt.attributes_json, "{}");

    return sink ? sink(&pt, user_data) : 0;
}

static int decode_metric(pb_reader_t *r, const metric_context_t *ctx,
                         pw_metric_sink_t sink, void *user_data, int *count) {
    char name[PW_METRIC_NAME_MAX] = "";
    char description[PW_METRIC_DESC_MAX] = "";
    char unit[PW_METRIC_UNIT_MAX] = "";

    // Gauge/sum/histogram payloads are length-delimited submessages that
    // reference name/description/unit; buffer them (rare to have more than a
    // couple) so field order on the wire cannot cost us those strings.
    pb_reader_t pending[8];
    const char *pending_kind[8];
    size_t pending_count = 0;

    pb_tag_t tag;
    while (pb_next(r, &tag) == 1) {
        switch (tag.field) {
            case F_METRIC_NAME:
                if (tag.wire == PB_WIRE_BYTES) {
                    const uint8_t *s; size_t n;
                    if (pb_bytes(r, &s, &n) != 1) return -1;
                    pw_copy_str(name, sizeof name, s, n);
                    continue;
                }
                break;
            case F_METRIC_DESCRIPTION:
                if (tag.wire == PB_WIRE_BYTES) {
                    const uint8_t *s; size_t n;
                    if (pb_bytes(r, &s, &n) != 1) return -1;
                    pw_copy_str(description, sizeof description, s, n);
                    continue;
                }
                break;
            case F_METRIC_UNIT:
                if (tag.wire == PB_WIRE_BYTES) {
                    const uint8_t *s; size_t n;
                    if (pb_bytes(r, &s, &n) != 1) return -1;
                    pw_copy_str(unit, sizeof unit, s, n);
                    continue;
                }
                break;
            case F_METRIC_GAUGE:
            case F_METRIC_SUM:
            case F_METRIC_HISTOGRAM:
                if (tag.wire == PB_WIRE_BYTES) {
                    pb_reader_t sub;
                    if (pb_submsg(r, &sub) != 1) return -1;
                    if (pending_count < sizeof pending / sizeof pending[0]) {
                        pending[pending_count] = sub;
                        pending_kind[pending_count] =
                            (tag.field == F_METRIC_GAUGE) ? "gauge" :
                            (tag.field == F_METRIC_SUM) ? "sum" : "histogram";
                        ++pending_count;
                    }
                    continue;
                }
                break;
            default:
                break;
        }
        if (pb_skip(r, tag.wire) != 1) return -1;
    }

    if (!name[0]) return 0; // unnamed metric, nothing useful to store

    for (size_t i = 0; i < pending_count; ++i) {
        pb_reader_t container = pending[i];
        int is_hist = strcmp(pending_kind[i], "histogram") == 0;
        pb_tag_t dtag;
        while (pb_next(&container, &dtag) == 1) {
            int is_points_field =
                (is_hist && dtag.field == F_HIST_DATA_POINTS) ||
                (!is_hist && dtag.field == F_GAUGE_DATA_POINTS); // == F_SUM_DATA_POINTS
            if (is_points_field && dtag.wire == PB_WIRE_BYTES) {
                pb_reader_t point;
                if (pb_submsg(&container, &point) != 1) return -1;
                int rc = is_hist
                    ? decode_histogram_point(&point, ctx, name, description, unit,
                                             sink, user_data)
                    : decode_number_point(&point, ctx, name, description, unit,
                                          pending_kind[i], sink, user_data);
                if (rc < 0) return -1;
                if (rc > 0) return 1; // sink asked to stop
                ++(*count);
            } else if (pb_skip(&container, dtag.wire) != 1) {
                return -1;
            }
        }
    }
    return 0;
}

static int decode_scope_metrics(pb_reader_t *r, metric_context_t *ctx,
                                pw_metric_sink_t sink, void *user_data, int *count) {
    pb_tag_t tag;
    while (pb_next(r, &tag) == 1) {
        if (tag.field == F_SM_SCOPE && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t scope;
            if (pb_submsg(r, &scope) != 1) return -1;
            pb_tag_t st;
            while (pb_next(&scope, &st) == 1) {
                if (st.field == F_IS_NAME && st.wire == PB_WIRE_BYTES) {
                    const uint8_t *s; size_t n;
                    if (pb_bytes(&scope, &s, &n) != 1) break;
                    pw_copy_str(ctx->scope_name, sizeof ctx->scope_name, s, n);
                } else if (pb_skip(&scope, st.wire) != 1) {
                    break;
                }
            }
        } else if (tag.field == F_SM_METRICS && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t metric;
            if (pb_submsg(r, &metric) != 1) return -1;
            int rc = decode_metric(&metric, ctx, sink, user_data, count);
            if (rc < 0) return -1;
            if (rc > 0) return 1;
        } else if (pb_skip(r, tag.wire) != 1) {
            return -1;
        }
    }
    return 0;
}

static int decode_resource_metrics(pb_reader_t *r, pw_metric_sink_t sink,
                                   void *user_data, int *count) {
    metric_context_t ctx;
    memset(&ctx, 0, sizeof ctx);
    snprintf(ctx.service_name, sizeof ctx.service_name, "unknown_service");

    // Same ordering caution as otlp_trace.c: resource precedes scope_metrics
    // in every producer we care about, but buffer scope_metrics anyway.
    pb_reader_t pending[64];
    size_t pending_count = 0;

    pb_tag_t tag;
    while (pb_next(r, &tag) == 1) {
        if (tag.field == F_RM_RESOURCE && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t res;
            if (pb_submsg(r, &res) != 1) return -1;
            pb_tag_t rt;
            while (pb_next(&res, &rt) == 1) {
                if (rt.field == F_RES_ATTRIBUTES && rt.wire == PB_WIRE_BYTES) {
                    pb_reader_t kv;
                    if (pb_submsg(&res, &kv) != 1) break;
                    char key[256];
                    pb_reader_t value;
                    int have_value;
                    pw_parse_kv(&kv, key, sizeof key, &value, &have_value);
                    if (have_value && strcmp(key, "service.name") == 0) {
                        pw_any_value_as_string(value, ctx.service_name,
                                              sizeof ctx.service_name);
                    } else if (have_value && strcmp(key, "procwatch.label") == 0) {
                        pw_any_value_as_string(value, ctx.label, sizeof ctx.label);
                    } else if (have_value && strcmp(key, "procwatch.pid_key") == 0) {
                        pw_any_value_as_string(value, ctx.pid_key, sizeof ctx.pid_key);
                    }
                } else if (pb_skip(&res, rt.wire) != 1) {
                    break;
                }
            }
        } else if (tag.field == F_RM_SCOPE_METRICS && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t sm;
            if (pb_submsg(r, &sm) != 1) return -1;
            if (pending_count < sizeof pending / sizeof pending[0]) {
                pending[pending_count++] = sm;
            } else {
                int rc = decode_scope_metrics(&sm, &ctx, sink, user_data, count);
                if (rc != 0) return rc;
            }
        } else if (pb_skip(r, tag.wire) != 1) {
            return -1;
        }
    }

    for (size_t i = 0; i < pending_count; ++i) {
        int rc = decode_scope_metrics(&pending[i], &ctx, sink, user_data, count);
        if (rc != 0) return rc;
    }
    return 0;
}

int otlp_decode_metrics(const void *data, size_t len,
                        pw_metric_sink_t sink, void *user_data) {
    pb_reader_t r;
    pb_reader_init(&r, data, len);

    int count = 0;
    pb_tag_t tag;
    while (pb_next(&r, &tag) == 1) {
        if (tag.field == F_EMSR_RESOURCE_METRICS && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t rm;
            if (pb_submsg(&r, &rm) != 1) return count ? count : -1;
            int rc = decode_resource_metrics(&rm, sink, user_data, &count);
            if (rc < 0) return count ? count : -1;
            if (rc > 0) return count;
        } else if (pb_skip(&r, tag.wire) != 1) {
            return count ? count : -1;
        }
    }
    return count;
}
