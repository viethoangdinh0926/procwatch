// Shared OTLP attribute (KeyValue/AnyValue) decoding, used by both the trace
// and metric decoders. See include/agent/otlp_common.h.

#include <stdio.h>
#include <string.h>

#include "../../include/agent/otlp_common.h"

void pw_sb_init(pw_sb_t *sb, char *buf, size_t cap) {
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->overflow = 0;
    if (cap) buf[0] = '\0';
}

void pw_sb_putc(pw_sb_t *sb, char c) {
    if (sb->len + 1 >= sb->cap) { sb->overflow = 1; return; }
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

void pw_sb_puts(pw_sb_t *sb, const char *s) {
    while (*s) pw_sb_putc(sb, *s++);
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

void pw_sb_json_string(pw_sb_t *sb, const char *s, size_t len) {
    pw_sb_putc(sb, '"');
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i];
        if (c == '"' || c == '\\') {
            pw_sb_putc(sb, '\\');
            pw_sb_putc(sb, (char)c);
            ++i;
        } else if (c == '\n') { pw_sb_puts(sb, "\\n"); ++i; }
        else if (c == '\r') { pw_sb_puts(sb, "\\r"); ++i; }
        else if (c == '\t') { pw_sb_puts(sb, "\\t"); ++i; }
        else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            pw_sb_puts(sb, esc);
            ++i;
        } else if (c < 0x80) {
            pw_sb_putc(sb, (char)c);
            ++i;
        } else {
            size_t n = utf8_seq_len(p + i, len - i);
            if (n == 0) {
                // Replace the offending byte with U+FFFD and resynchronise.
                pw_sb_puts(sb, "\\ufffd");
                ++i;
            } else {
                for (size_t k = 0; k < n; ++k) pw_sb_putc(sb, (char)p[i + k]);
                i += n;
            }
        }
    }
    pw_sb_putc(sb, '"');
}

void pw_copy_str(char *dst, size_t cap, const uint8_t *src, size_t len) {
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void pw_to_hex(char *dst, size_t dst_cap, const uint8_t *src, size_t len) {
    static const char digits[] = "0123456789abcdef";
    size_t max = (dst_cap - 1) / 2;
    if (len > max) len = max;
    for (size_t i = 0; i < len; ++i) {
        dst[i * 2] = digits[src[i] >> 4];
        dst[i * 2 + 1] = digits[src[i] & 0x0f];
    }
    dst[len * 2] = '\0';
}

static void decode_any_value(pb_reader_t *r, pw_sb_t *sb, int depth);

void pw_parse_kv(pb_reader_t *kv, char *key, size_t key_cap,
                 pb_reader_t *value_out, int *have_value) {
    key[0] = '\0';
    *have_value = 0;

    pb_tag_t tag;
    while (pb_next(kv, &tag) == 1) {
        if (tag.field == F_KV_KEY && tag.wire == PB_WIRE_BYTES) {
            const uint8_t *s; size_t n;
            if (pb_bytes(kv, &s, &n) != 1) return;
            pw_copy_str(key, key_cap, s, n);
        } else if (tag.field == F_KV_VALUE && tag.wire == PB_WIRE_BYTES) {
            if (pb_submsg(kv, value_out) != 1) return;
            *have_value = 1;
        } else if (pb_skip(kv, tag.wire) != 1) {
            return;
        }
    }
}

int pw_any_value_as_string(pb_reader_t value, char *out, size_t cap) {
    pb_tag_t tag;
    while (pb_next(&value, &tag) == 1) {
        if (tag.field == F_AV_STRING && tag.wire == PB_WIRE_BYTES) {
            const uint8_t *s; size_t n;
            if (pb_bytes(&value, &s, &n) != 1) return 0;
            pw_copy_str(out, cap, s, n);
            return 1;
        }
        if (pb_skip(&value, tag.wire) != 1) return 0;
    }
    return 0;
}

static void decode_value_list(pb_reader_t *r, pw_sb_t *sb, int depth) {
    pw_sb_putc(sb, '[');
    int first = 1;
    pb_tag_t tag;
    int rc;
    while ((rc = pb_next(r, &tag)) == 1) {
        if (tag.field == F_LIST_VALUES && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t sub;
            if (pb_submsg(r, &sub) != 1) break;
            if (!first) pw_sb_putc(sb, ',');
            first = 0;
            decode_any_value(&sub, sb, depth + 1);
        } else if (pb_skip(r, tag.wire) != 1) {
            break;
        }
    }
    pw_sb_putc(sb, ']');
}

static void decode_kvlist(pb_reader_t *r, pw_sb_t *sb, int depth) {
    pw_sb_putc(sb, '{');
    int first = 1;
    pb_tag_t tag;
    while (pb_next(r, &tag) == 1) {
        if (tag.field == F_LIST_VALUES && tag.wire == PB_WIRE_BYTES) {
            pb_reader_t kv;
            if (pb_submsg(r, &kv) != 1) break;
            size_t mark = sb->len;
            if (!first) pw_sb_putc(sb, ',');
            if (!pw_append_kv_json(&kv, sb, depth)) {
                sb->len = mark;
                sb->buf[mark] = '\0';
            } else {
                first = 0;
            }
        } else if (pb_skip(r, tag.wire) != 1) {
            break;
        }
    }
    pw_sb_putc(sb, '}');
}

static void decode_any_value(pb_reader_t *r, pw_sb_t *sb, int depth) {
    if (depth > PW_ATTR_MAX_DEPTH) { pw_sb_puts(sb, "null"); return; }

    pb_tag_t tag;
    int rc;
    int wrote = 0;
    while ((rc = pb_next(r, &tag)) == 1) {
        if (tag.field == F_AV_STRING && tag.wire == PB_WIRE_BYTES) {
            const uint8_t *s; size_t n;
            if (pb_bytes(r, &s, &n) != 1) break;
            pw_sb_json_string(sb, (const char *)s, n);
            wrote = 1;
        } else if (tag.field == F_AV_BOOL && tag.wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (pb_varint(r, &v) != 1) break;
            pw_sb_puts(sb, v ? "true" : "false");
            wrote = 1;
        } else if (tag.field == F_AV_INT && tag.wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (pb_varint(r, &v) != 1) break;
            // int64, so a plain sign-extended varint. No zigzag: negative
            // values arrive as the full ten bytes and reinterpret directly.
            char num[32];
            snprintf(num, sizeof num, "%lld", (long long)(int64_t)v);
            pw_sb_puts(sb, num);
            wrote = 1;
        } else if (tag.field == F_AV_DOUBLE && tag.wire == PB_WIRE_FIXED64) {
            uint64_t bits;
            if (pb_fixed64(r, &bits) != 1) break;
            char num[64];
            snprintf(num, sizeof num, "%.17g", pb_bits_to_double(bits));
            pw_sb_puts(sb, num);
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
            pw_to_hex(hex, sizeof hex, s, n);
            pw_sb_json_string(sb, hex, strlen(hex));
            wrote = 1;
        } else if (pb_skip(r, tag.wire) != 1) {
            break;
        }
    }
    if (!wrote) pw_sb_puts(sb, "null");
}

int pw_append_kv_json(pb_reader_t *kv, pw_sb_t *sb, int depth) {
    char key[256];
    pb_reader_t value;
    int have_value;
    pw_parse_kv(kv, key, sizeof key, &value, &have_value);
    if (!key[0]) return 0;

    pw_sb_json_string(sb, key, strlen(key));
    pw_sb_putc(sb, ':');
    if (have_value) decode_any_value(&value, sb, depth + 1);
    else pw_sb_puts(sb, "null");
    return 1;
}
