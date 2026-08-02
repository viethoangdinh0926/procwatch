#include <string.h>

#include "../../include/agent/pb_decode.h"

#define PB_VARINT_MAX_BYTES 10

void pb_reader_init(pb_reader_t *r, const void *data, size_t len) {
    r->ptr = (const uint8_t *)data;
    r->end = r->ptr + len;
}

int pb_varint(pb_reader_t *r, uint64_t *out) {
    uint64_t value = 0;
    int shift = 0;
    for (int i = 0; i < PB_VARINT_MAX_BYTES; ++i) {
        if (r->ptr >= r->end) return -1;
        uint8_t byte = *r->ptr++;
        value |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            if (out) *out = value;
            return 1;
        }
        shift += 7;
    }
    // More than ten continuation bytes cannot describe a 64-bit value.
    return -1;
}

int pb_next(pb_reader_t *r, pb_tag_t *tag) {
    if (r->ptr >= r->end) return 0;

    uint64_t key;
    // The tag is itself a varint, so field numbers above 15 occupy more than
    // one byte. Span.flags is field 16, whose tag 133 encodes as 0x85 0x01;
    // reading a single byte there would desynchronise the whole parse.
    if (pb_varint(r, &key) != 1) return -1;

    uint32_t wire = (uint32_t)(key & 0x07);
    uint32_t field = (uint32_t)(key >> 3);
    if (field == 0) return -1;

    // Wire types 3 and 4 are the deprecated group encoding, and 6/7 are
    // undefined. OTLP uses none of them.
    if (wire != PB_WIRE_VARINT && wire != PB_WIRE_FIXED64 &&
        wire != PB_WIRE_BYTES && wire != PB_WIRE_FIXED32) {
        return -1;
    }

    tag->field = field;
    tag->wire = wire;
    return 1;
}

int pb_fixed32(pb_reader_t *r, uint32_t *out) {
    if (r->end - r->ptr < 4) return -1;
    uint32_t value = (uint32_t)r->ptr[0] |
                     ((uint32_t)r->ptr[1] << 8) |
                     ((uint32_t)r->ptr[2] << 16) |
                     ((uint32_t)r->ptr[3] << 24);
    r->ptr += 4;
    if (out) *out = value;
    return 1;
}

int pb_fixed64(pb_reader_t *r, uint64_t *out) {
    if (r->end - r->ptr < 8) return -1;
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | r->ptr[i];
    r->ptr += 8;
    if (out) *out = value;
    return 1;
}

int pb_bytes(pb_reader_t *r, const uint8_t **out, size_t *out_len) {
    uint64_t len;
    if (pb_varint(r, &len) != 1) return -1;
    if ((uint64_t)(r->end - r->ptr) < len) return -1;
    if (out) *out = r->ptr;
    if (out_len) *out_len = (size_t)len;
    r->ptr += len;
    return 1;
}

int pb_submsg(pb_reader_t *r, pb_reader_t *sub) {
    const uint8_t *data;
    size_t len;
    if (pb_bytes(r, &data, &len) != 1) return -1;
    pb_reader_init(sub, data, len);
    return 1;
}

int pb_skip(pb_reader_t *r, uint32_t wire) {
    switch (wire) {
        case PB_WIRE_VARINT:  return pb_varint(r, NULL);
        case PB_WIRE_FIXED64: return pb_fixed64(r, NULL);
        case PB_WIRE_FIXED32: return pb_fixed32(r, NULL);
        case PB_WIRE_BYTES:   return pb_bytes(r, NULL, NULL);
        default:              return -1;
    }
}

double pb_bits_to_double(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}
