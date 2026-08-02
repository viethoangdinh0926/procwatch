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

// KeyValue
#define F_KV_KEY 1
#define F_KV_VALUE 2

// AnyValue
#define F_AV_STRING 1
#define F_AV_BOOL 2
#define F_AV_INT 3
#define F_AV_DOUBLE 4
#define F_AV_ARRAY 5
#define F_AV_KVLIST 6
#define F_AV_BYTES 7

// ArrayValue.values / KeyValueList.values
#define F_LIST_VALUES 1

#define ATTR_MAX_DEPTH 4

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

// ------------------------ Bounded string builder ------------------------

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int overflow;
} sb_t;

static void sb_init(sb_t *sb, char *buf, size_t cap) {
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->overflow = 0;
    if (cap) buf[0] = '\0';
}

static void sb_putc(sb_t *sb, char c) {
    if (sb->len + 1 >= sb->cap) { sb->overflow = 1; return; }
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

static void sb_puts(sb_t *sb, const char *s) {
    while (*s) sb_putc(sb, *s++);
}

// Length of the UTF-8 sequence starting at s, or 0 if it is not valid UTF-8.
// Postgres rejects invalid UTF-8 in jsonb outright, so an unvalidated
// passthrough would fail the whole INSERT rather than one attribute.
static size_t utf8_seq_len(const unsigned char *s, size_t avail) {
    unsigned char c = s[0];
    size_t need;
    if (c < 0x80) return 1;
    else if ((c & 0xe0) == 0xc0) need = 2;
    else if ((c & 0xf0) == 0xe0) need = 3;
    else if ((c & 0xf8) == 0xf0) need = 4;
    else return 0;

    if (avail < need) return 0;
    for (size_t i = 1; i < need; ++i) {
        if ((s[i] & 0xc0) != 0x80) return 0;
    }
    // Reject overlong encodings and anything past U+10FFFF.
    if (need == 2 && c < 0xc2) return 0;
    if (need == 3 && c == 0xe0 && s[1] < 0xa0) return 0;
    if (need == 4 && (c > 0xf4 || (c == 0xf0 && s[1] < 0x90))) return 0;
    return need;
}

static void sb_json_string(sb_t *sb, const char *s, size_t len) {
    sb_putc(sb, '"');
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i];
        if (c == '"' || c == '\\') {
            sb_putc(sb, '\\');
            sb_putc(sb, (char)c);
            ++i;
        } else if (c == '\n') { sb_puts(sb, "\\n"); ++i; }
        else if (c == '\r') { sb_puts(sb, "\\r"); ++i; }
        else if (c == '\t') { sb_puts(sb, "\\t"); ++i; }
        else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            sb_puts(sb, esc);
            ++i;
        } else if (c < 0x80) {
            sb_putc(sb, (char)c);
            ++i;
        } else {
            size_t n = utf8_seq_len(p + i, len - i);
            if (n == 0) {
                // Replace the offending byte with U+FFFD and resynchronise.
                sb_puts(sb, "\\ufffd");
                ++i;
            } else {
                for (size_t k = 0; k < n; ++k) sb_putc(sb, (char)p[i + k]);
                i += n;
            }
        }
    }
    sb_putc(sb, '"');
}

// ------------------------ Field helpers ------------------------

static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t len) {
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void to_hex(char *dst, size_t dst_cap, const uint8_t *src, size_t len) {
    static const char digits[] = "0123456789abcdef";
    size_t max = (dst_cap - 1) / 2;
    if (len > max) len = max;
    for (size_t i = 0; i < len; ++i) {
        dst[i * 2] = digits[src[i] >> 4];
        dst[i * 2 + 1] = digits[src[i] & 0x0f];
    }
    dst[len * 2] = '\0';
}

static void decode_any_value(pb_reader_t *r, sb_t *sb, int depth);

// Splits one KeyValue submessage into its key and a reader positioned at its
// AnyValue. Shared by span attributes, resource attributes, and kvlists.
static void parse_kv(pb_reader_t *kv, char *key, size_t key_cap,
                     pb_reader_t *value_out, int *have_value) {
    key[0] = '\0';
    *have_value = 0;

    pb_tag_t tag;
    while (pb_next(kv, &tag) == 1) {
        if (tag.field == F_KV_KEY && tag.wire == PB_WIRE_BYTES) {
            const uint8_t *s; size_t n;
            if (pb_bytes(kv, &s, &n) != 1) return;
            copy_str(key, key_cap, s, n);
        } else if (tag.field == F_KV_VALUE && tag.wire == PB_WIRE_BYTES) {
            if (pb_submsg(kv, value_out) != 1) return;
            *have_value = 1;
        } else if (pb_skip(kv, tag.wire) != 1) {
            return;
        }
    }
}

// Pulls the string form of an AnyValue, used for resource attributes that get
// promoted to their own column. Non-string values yield nothing.
static int any_value_as_string(pb_reader_t value, char *out, size_t cap) {
    pb_tag_t tag;
    while (pb_next(&value, &tag) == 1) {
        if (tag.field == F_AV_STRING && tag.wire == PB_WIRE_BYTES) {
            const uint8_t *s; size_t n;
            if (pb_bytes(&value, &s, &n) != 1) return 0;
            copy_str(out, cap, s, n);
            return 1;
        }
        if (pb_skip(&value, tag.wire) != 1) return 0;
    }
    return 0;
}

static void decode_value_list(pb_reader_t *r, sb_t *sb, int depth) {
    sb_putc(sb, '[');
    int first = 1;
    pb_tag_t tag;
    int rc;
    while ((rc = pb_next(r, &tag)) == 1) {
        if (tag.field == F_LIST_VALUES && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t sub;
            if (pb_submsg(r, &sub) != 1) break;
            if (!first) sb_putc(sb, ',');
            first = 0;
            decode_any_value(&sub, sb, depth + 1);
        } else if (pb_skip(r, tag.wire) != 1) {
            break;
        }
    }
    sb_putc(sb, ']');
}

static void decode_kvlist(pb_reader_t *r, sb_t *sb, int depth);

static void decode_any_value(pb_reader_t *r, sb_t *sb, int depth) {
    if (depth > ATTR_MAX_DEPTH) { sb_puts(sb, "null"); return; }

    pb_tag_t tag;
    int rc;
    int wrote = 0;
    while ((rc = pb_next(r, &tag)) == 1) {
        if (tag.field == F_AV_STRING && tag.wire == PB_WIRE_BYTES) {
            const uint8_t *s; size_t n;
            if (pb_bytes(r, &s, &n) != 1) break;
            sb_json_string(sb, (const char *)s, n);
            wrote = 1;
        } else if (tag.field == F_AV_BOOL && tag.wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (pb_varint(r, &v) != 1) break;
            sb_puts(sb, v ? "true" : "false");
            wrote = 1;
        } else if (tag.field == F_AV_INT && tag.wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (pb_varint(r, &v) != 1) break;
            // int64, so a plain sign-extended varint. No zigzag: negative
            // values arrive as the full ten bytes and reinterpret directly.
            char num[32];
            snprintf(num, sizeof num, "%lld", (long long)(int64_t)v);
            sb_puts(sb, num);
            wrote = 1;
        } else if (tag.field == F_AV_DOUBLE && tag.wire == PB_WIRE_FIXED64) {
            uint64_t bits;
            if (pb_fixed64(r, &bits) != 1) break;
            char num[64];
            snprintf(num, sizeof num, "%.17g", pb_bits_to_double(bits));
            sb_puts(sb, num);
            wrote = 1;
        } else if (tag.field == F_AV_ARRAY && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t sub;
            if (pb_submsg(r, &sub) != 1) break;
            decode_value_list(&sub, sb, depth);
            wrote = 1;
        } else if (tag.field == F_AV_KVLIST && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t sub;
            if (pb_submsg(r, &sub) != 1) break;
            decode_kvlist(&sub, sb, depth);
            wrote = 1;
        } else if (tag.field == F_AV_BYTES && tag.wire == PB_WIRE_BYTES) {
            const uint8_t *s; size_t n;
            if (pb_bytes(r, &s, &n) != 1) break;
            char hex[129];
            to_hex(hex, sizeof hex, s, n);
            sb_json_string(sb, hex, strlen(hex));
            wrote = 1;
        } else if (pb_skip(r, tag.wire) != 1) {
            break;
        }
    }
    if (!wrote) sb_puts(sb, "null");
}

// Appends `"key":value` for one KeyValue submessage. Returns 1 if anything
// was written, so the caller knows whether a separator is needed next.
static int append_kv_json(pb_reader_t *kv, sb_t *sb, int depth) {
    char key[256];
    pb_reader_t value;
    int have_value;
    parse_kv(kv, key, sizeof key, &value, &have_value);
    if (!key[0]) return 0;

    sb_json_string(sb, key, strlen(key));
    sb_putc(sb, ':');
    if (have_value) decode_any_value(&value, sb, depth + 1);
    else sb_puts(sb, "null");
    return 1;
}

static void decode_kvlist(pb_reader_t *r, sb_t *sb, int depth) {
    sb_putc(sb, '{');
    int first = 1;
    pb_tag_t tag;
    while (pb_next(r, &tag) == 1) {
        if (tag.field == F_LIST_VALUES && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t kv;
            if (pb_submsg(r, &kv) != 1) break;
            size_t mark = sb->len;
            if (!first) sb_putc(sb, ',');
            if (!append_kv_json(&kv, sb, depth)) {
                sb->len = mark;
                sb->buf[mark] = '\0';
            } else {
                first = 0;
            }
        } else if (pb_skip(r, tag.wire) != 1) {
            break;
        }
    }
    sb_putc(sb, '}');
}

// ------------------------ Span walk ------------------------

typedef struct {
    char service_name[PW_SPAN_SERVICE_MAX];
    char scope_name[PW_SPAN_SCOPE_MAX];
} span_context_t;

static int decode_span(pb_reader_t *r, const span_context_t *ctx,
                       pw_span_sink_t sink, void *user_data) {
    pw_span_t span;
    memset(&span, 0, sizeof span);
    span.kind = span_kind_name(0);
    span.status_code = status_code_name(0);
    snprintf(span.service_name, sizeof span.service_name, "%s", ctx->service_name);
    snprintf(span.scope_name, sizeof span.scope_name, "%s", ctx->scope_name);

    sb_t attrs;
    sb_init(&attrs, span.attributes_json, sizeof span.attributes_json);
    sb_putc(&attrs, '{');
    int first_attr = 1;
    int have_attrs = 0;

    pb_tag_t tag;
    while (pb_next(r, &tag) == 1) {
        switch (tag.field) {
            case F_SPAN_TRACE_ID:
                if (tag.wire == PB_WIRE_BYTES) {
                    const uint8_t *s; size_t n;
                    if (pb_bytes(r, &s, &n) != 1) return -1;
                    to_hex(span.trace_id, sizeof span.trace_id, s, n);
                    continue;
                }
                break;
            case F_SPAN_SPAN_ID:
                if (tag.wire == PB_WIRE_BYTES) {
                    const uint8_t *s; size_t n;
                    if (pb_bytes(r, &s, &n) != 1) return -1;
                    to_hex(span.span_id, sizeof span.span_id, s, n);
                    continue;
                }
                break;
            case F_SPAN_PARENT_SPAN_ID:
                if (tag.wire == PB_WIRE_BYTES) {
                    const uint8_t *s; size_t n;
                    if (pb_bytes(r, &s, &n) != 1) return -1;
                    to_hex(span.parent_span_id, sizeof span.parent_span_id, s, n);
                    continue;
                }
                break;
            case F_SPAN_NAME:
                if (tag.wire == PB_WIRE_BYTES) {
                    const uint8_t *s; size_t n;
                    if (pb_bytes(r, &s, &n) != 1) return -1;
                    copy_str(span.name, sizeof span.name, s, n);
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
                    if (!first_attr) sb_putc(&attrs, ',');
                    if (append_kv_json(&kv, &attrs, 0)) {
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
                            copy_str(span.status_message, sizeof span.status_message, s, n);
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

    sb_putc(&attrs, '}');
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
                    copy_str(ctx->scope_name, sizeof ctx->scope_name, s, n);
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
                    parse_kv(&kv, key, sizeof key, &value, &have_value);
                    if (have_value && strcmp(key, "service.name") == 0) {
                        any_value_as_string(value, ctx.service_name,
                                            sizeof ctx.service_name);
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
