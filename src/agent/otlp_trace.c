// Decodes OTLP ExportTraceServiceRequest payloads into flat span records.
//
// Field numbers below are taken from opentelemetry-proto. Three of them are
// easy to get wrong and produce silent corruption rather than an error:
//   - Span.flags is field 16, so its tag needs a multi-byte varint read.
//     pb_next handles that; a hand-rolled single-byte read would not.
//   - Status reserves field 1, so message is 2 and code is 3.
//   - Timestamps are fixed64 and flags is fixed32, but every dropped_*_count
//     is a varint despite being the same width in C.

#include <stdio.h>
#include <string.h>

#include "../../include/agent/pb_decode.h"
#include "../../include/agent/otlp.h"
#include "../../include/agent/otlp_common.h"

// ExportTraceServiceRequest
#define F_ETSR_RESOURCE_SPANS 1

// ResourceSpans
#define F_RS_RESOURCE 1
#define F_RS_SCOPE_SPANS 2

// Resource
#define F_RES_ATTRIBUTES 1

// ScopeSpans
#define F_SS_SCOPE 1
#define F_SS_SPANS 2

// InstrumentationScope
#define F_IS_NAME 1

// Span
#define F_SPAN_TRACE_ID 1
#define F_SPAN_SPAN_ID 2
#define F_SPAN_PARENT_SPAN_ID 4
#define F_SPAN_NAME 5
#define F_SPAN_KIND 6
#define F_SPAN_START 7
#define F_SPAN_END 8
#define F_SPAN_ATTRIBUTES 9
#define F_SPAN_STATUS 15

// Status: field 1 is reserved.
#define F_STATUS_MESSAGE 2
#define F_STATUS_CODE 3

static const char *span_kind_name(uint64_t kind) {
    switch (kind) {
        case 1: return "INTERNAL";
        case 2: return "SERVER";
        case 3: return "CLIENT";
        case 4: return "PRODUCER";
        case 5: return "CONSUMER";
        default: return "UNSPECIFIED";
    }
}

static const char *status_code_name(uint64_t code) {
    switch (code) {
        case 1: return "OK";
        case 2: return "ERROR";
        default: return "UNSET";
    }
}

// ------------------------ Span walk ------------------------

typedef struct {
    char service_name[PW_SPAN_SERVICE_MAX];
    char scope_name[PW_SPAN_SCOPE_MAX];
    char label[PW_SPAN_LABEL_MAX];
} span_context_t;

static int decode_span(pb_reader_t *r, const span_context_t *ctx,
                       pw_span_sink_t sink, void *user_data) {
    pw_span_t span;
    memset(&span, 0, sizeof span);
    span.kind = span_kind_name(0);
    span.status_code = status_code_name(0);
    snprintf(span.service_name, sizeof span.service_name, "%s", ctx->service_name);
    snprintf(span.scope_name, sizeof span.scope_name, "%s", ctx->scope_name);
    snprintf(span.label, sizeof span.label, "%s", ctx->label);

    pw_sb_t attrs;
    pw_sb_init(&attrs, span.attributes_json, sizeof span.attributes_json);
    pw_sb_putc(&attrs, '{');
    int first_attr = 1;
    int have_attrs = 0;

    pb_tag_t tag;
    while (pb_next(r, &tag) == 1) {
        switch (tag.field) {
            case F_SPAN_TRACE_ID:
                if (tag.wire == PB_WIRE_BYTES) {
                    const uint8_t *s; size_t n;
                    if (pb_bytes(r, &s, &n) != 1) return -1;
                    pw_to_hex(span.trace_id, sizeof span.trace_id, s, n);
                    continue;
                }
                break;
            case F_SPAN_SPAN_ID:
                if (tag.wire == PB_WIRE_BYTES) {
                    const uint8_t *s; size_t n;
                    if (pb_bytes(r, &s, &n) != 1) return -1;
                    pw_to_hex(span.span_id, sizeof span.span_id, s, n);
                    continue;
                }
                break;
            case F_SPAN_PARENT_SPAN_ID:
                if (tag.wire == PB_WIRE_BYTES) {
                    const uint8_t *s; size_t n;
                    if (pb_bytes(r, &s, &n) != 1) return -1;
                    pw_to_hex(span.parent_span_id, sizeof span.parent_span_id, s, n);
                    continue;
                }
                break;
            case F_SPAN_NAME:
                if (tag.wire == PB_WIRE_BYTES) {
                    const uint8_t *s; size_t n;
                    if (pb_bytes(r, &s, &n) != 1) return -1;
                    pw_copy_str(span.name, sizeof span.name, s, n);
                    continue;
                }
                break;
            case F_SPAN_KIND:
                if (tag.wire == PB_WIRE_VARINT) {
                    uint64_t v;
                    if (pb_varint(r, &v) != 1) return -1;
                    span.kind = span_kind_name(v);
                    continue;
                }
                break;
            case F_SPAN_START:
                if (tag.wire == PB_WIRE_FIXED64) {
                    if (pb_fixed64(r, &span.start_ns) != 1) return -1;
                    continue;
                }
                break;
            case F_SPAN_END:
                if (tag.wire == PB_WIRE_FIXED64) {
                    if (pb_fixed64(r, &span.end_ns) != 1) return -1;
                    continue;
                }
                break;
            case F_SPAN_ATTRIBUTES:
                if (tag.wire == PB_WIRE_BYTES) {
                    pb_reader_t kv;
                    if (pb_submsg(r, &kv) != 1) return -1;
                    size_t mark = attrs.len;
                    if (!first_attr) pw_sb_putc(&attrs, ',');
                    if (pw_append_kv_json(&kv, &attrs, 0)) {
                        first_attr = 0;
                        have_attrs = 1;
                    } else {
                        // Roll back the separator we optimistically wrote.
                        attrs.len = mark;
                        attrs.buf[mark] = '\0';
                    }
                    continue;
                }
                break;
            case F_SPAN_STATUS:
                if (tag.wire == PB_WIRE_BYTES) {
                    pb_reader_t st;
                    if (pb_submsg(r, &st) != 1) return -1;
                    pb_tag_t stt;
                    while (pb_next(&st, &stt) == 1) {
                        if (stt.field == F_STATUS_MESSAGE && stt.wire == PB_WIRE_BYTES) {
                            const uint8_t *s; size_t n;
                            if (pb_bytes(&st, &s, &n) != 1) break;
                            pw_copy_str(span.status_message, sizeof span.status_message, s, n);
                        } else if (stt.field == F_STATUS_CODE && stt.wire == PB_WIRE_VARINT) {
                            uint64_t v;
                            if (pb_varint(&st, &v) != 1) break;
                            span.status_code = status_code_name(v);
                        } else if (pb_skip(&st, stt.wire) != 1) {
                            break;
                        }
                    }
                    continue;
                }
                break;
            default:
                break;
        }
        if (pb_skip(r, tag.wire) != 1) return -1;
    }

    pw_sb_putc(&attrs, '}');
    if (!have_attrs) snprintf(span.attributes_json, sizeof span.attributes_json, "{}");

    span.duration_ns = (span.end_ns >= span.start_ns)
                     ? (int64_t)(span.end_ns - span.start_ns)
                     : 0;

    return sink ? sink(&span, user_data) : 0;
}

static int decode_scope_spans(pb_reader_t *r, span_context_t *ctx,
                              pw_span_sink_t sink, void *user_data, int *count) {
    pb_tag_t tag;
    while (pb_next(r, &tag) == 1) {
        if (tag.field == F_SS_SCOPE && tag.wire == PB_WIRE_BYTES) {
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
        } else if (tag.field == F_SS_SPANS && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t span;
            if (pb_submsg(r, &span) != 1) return -1;
            int rc = decode_span(&span, ctx, sink, user_data);
            if (rc < 0) return -1;
            if (rc > 0) return 1; // sink asked to stop
            ++(*count);
        } else if (pb_skip(r, tag.wire) != 1) {
            return -1;
        }
    }
    return 0;
}

static int decode_resource_spans(pb_reader_t *r, pw_span_sink_t sink,
                                 void *user_data, int *count) {
    span_context_t ctx;
    memset(&ctx, 0, sizeof ctx);
    snprintf(ctx.service_name, sizeof ctx.service_name, "unknown_service");

    // ResourceSpans.resource precedes scope_spans on the wire in every
    // producer we care about, but buffer the scope_spans submessages anyway
    // so ordering cannot cost us the service name.
    pb_reader_t pending[64];
    size_t pending_count = 0;

    pb_tag_t tag;
    while (pb_next(r, &tag) == 1) {
        if (tag.field == F_RS_RESOURCE && tag.wire == PB_WIRE_BYTES) {
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
                    }
                } else if (pb_skip(&res, rt.wire) != 1) {
                    break;
                }
            }
        } else if (tag.field == F_RS_SCOPE_SPANS && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t ss;
            if (pb_submsg(r, &ss) != 1) return -1;
            if (pending_count < sizeof pending / sizeof pending[0]) {
                pending[pending_count++] = ss;
            } else {
                // More scopes than we can buffer: decode immediately rather
                // than drop them.
                int rc = decode_scope_spans(&ss, &ctx, sink, user_data, count);
                if (rc != 0) return rc;
            }
        } else if (pb_skip(r, tag.wire) != 1) {
            return -1;
        }
    }

    for (size_t i = 0; i < pending_count; ++i) {
        int rc = decode_scope_spans(&pending[i], &ctx, sink, user_data, count);
        if (rc != 0) return rc;
    }
    return 0;
}

int otlp_decode_traces(const void *data, size_t len,
                       pw_span_sink_t sink, void *user_data) {
    pb_reader_t r;
    pb_reader_init(&r, data, len);

    int count = 0;
    pb_tag_t tag;
    while (pb_next(&r, &tag) == 1) {
        if (tag.field == F_ETSR_RESOURCE_SPANS && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t rs;
            if (pb_submsg(&r, &rs) != 1) return count ? count : -1;
            int rc = decode_resource_spans(&rs, sink, user_data, &count);
            if (rc < 0) return count ? count : -1;
            if (rc > 0) return count;
        } else if (pb_skip(&r, tag.wire) != 1) {
            return count ? count : -1;
        }
    }
    return count;
}
