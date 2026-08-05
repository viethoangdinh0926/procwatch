#pragma once

// Shared OTLP wire-format helpers used by both the trace and metric
// decoders: the KeyValue/AnyValue attribute encoding is identical across
// every OTLP signal, so it is decoded once here rather than duplicated.

#include <stddef.h>
#include <stdint.h>

#include "pb_decode.h"

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

#define PW_ATTR_MAX_DEPTH 4

// Bounded string builder: appends are no-ops past cap, tracked via overflow
// so callers can roll back optimistic writes (e.g. a separator before a
// field that turned out to be unparsable).
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int overflow;
} pw_sb_t;

void pw_sb_init(pw_sb_t *sb, char *buf, size_t cap);
void pw_sb_putc(pw_sb_t *sb, char c);
void pw_sb_puts(pw_sb_t *sb, const char *s);
void pw_sb_json_string(pw_sb_t *sb, const char *s, size_t len);

void pw_copy_str(char *dst, size_t cap, const uint8_t *src, size_t len);
void pw_to_hex(char *dst, size_t dst_cap, const uint8_t *src, size_t len);

// Splits one KeyValue submessage into its key and a reader positioned at its
// AnyValue.
void pw_parse_kv(pb_reader_t *kv, char *key, size_t key_cap,
                 pb_reader_t *value_out, int *have_value);

// Pulls the string form of an AnyValue. Non-string values yield nothing.
int pw_any_value_as_string(pb_reader_t value, char *out, size_t cap);

// Appends `"key":value` for one KeyValue submessage. Returns 1 if anything
// was written, so the caller knows whether a separator is needed next.
int pw_append_kv_json(pb_reader_t *kv, pw_sb_t *sb, int depth);
