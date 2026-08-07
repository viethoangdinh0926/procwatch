#pragma once

#include <stddef.h>
#include <stdint.h>

// Optional best-effort OTLP/HTTP forward of raw protobuf bodies (Tempo traces,
// Prometheus OTLP metrics). libc sockets only; http:// only (no TLS).

// Returns 1 if url is a usable http://host[:port][/path] endpoint, else 0.
// Empty/NULL and non-http schemes are invalid (forwarding stays disabled).
int otlp_forward_url_valid(const char *url);

// POSTs body to url with Content-Type: application/x-protobuf.
// Short connect/IO timeout. Returns 0 on HTTP 2xx, -1 on any failure.
// Callers that treat forwarding as optional should ignore the return value.
int otlp_forward_post(const char *url, const uint8_t *body, size_t body_len);
