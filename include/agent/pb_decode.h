#pragma once

// Minimal protobuf wire-format reader.
//
// OTLP is the only schema we consume and we need a small subset of its
// fields, so this walks the wire format directly rather than pulling in
// protobuf-c or generated code. The wire format carries no field names or
// types, only field numbers and one of four wire types, which is all a
// reader needs to skip what it does not recognise.

#include <stddef.h>
#include <stdint.h>

#define PB_WIRE_VARINT 0
#define PB_WIRE_FIXED64 1
#define PB_WIRE_BYTES 2
#define PB_WIRE_FIXED32 5

typedef struct {
    const uint8_t *ptr;
    const uint8_t *end;
} pb_reader_t;

typedef struct {
    uint32_t field;
    uint32_t wire;
} pb_tag_t;

void pb_reader_init(pb_reader_t *r, const void *data, size_t len);

// Reads the next tag. Returns 1 on success, 0 at end of buffer, -1 on a
// malformed or unsupported (group) tag.
int pb_next(pb_reader_t *r, pb_tag_t *tag);

// Each returns 1 on success and -1 if the field runs past the end of the
// buffer. Callers must match the reader to the field's declared wire type;
// pb_skip handles anything unrecognised.
int pb_varint(pb_reader_t *r, uint64_t *out);
int pb_fixed32(pb_reader_t *r, uint32_t *out);
int pb_fixed64(pb_reader_t *r, uint64_t *out);

// Borrows a pointer into the caller's buffer; nothing is copied or owned.
int pb_bytes(pb_reader_t *r, const uint8_t **out, size_t *out_len);

// Length-delimited field reinterpreted as a nested message.
int pb_submsg(pb_reader_t *r, pb_reader_t *sub);

int pb_skip(pb_reader_t *r, uint32_t wire);

// double_value is transmitted as fixed64 holding an IEEE-754 bit pattern.
double pb_bits_to_double(uint64_t bits);
