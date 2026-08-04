#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define PW_HTTP_MAX_CONNS 128
#define PW_HTTP_PATH_MAX 128

// Signal kinds the OTLP/HTTP receiver understands, taken from the request
// path. Metrics and logs are accepted and acknowledged so that an SDK
// configured to export all three does not spew connection errors.
typedef enum {
    PW_SIGNAL_UNKNOWN = 0,
    PW_SIGNAL_TRACES,
    PW_SIGNAL_METRICS,
    PW_SIGNAL_LOGS,
    PW_SIGNAL_PROCMETRICS
} pw_signal_t;

typedef struct {
    pw_signal_t signal;
    const char *path;
    const uint8_t *body;
    size_t body_len;
} pw_http_request_t;

// Return 0 to acknowledge with 200, non-zero to reply 400.
typedef int (*pw_http_handler_t)(const pw_http_request_t *req, void *user_data);

typedef struct {
    int fd;
    uint8_t *buf;
    size_t len;
    size_t cap;
    int headers_done;
    size_t body_offset;
    size_t content_length;
    int gzip;
    int keep_alive;
    char path[PW_HTTP_PATH_MAX];
    time_t last_active;
} pw_conn_t;

typedef struct {
    int listen_fd;
    pw_conn_t conns[PW_HTTP_MAX_CONNS];
    size_t max_body;
    int idle_timeout_sec;
    pw_http_handler_t handler;
    void *user_data;
    unsigned long long requests;
    unsigned long long rejected;
} pw_http_server_t;

// Binds and listens. Returns 0 on success, -1 on failure (message on stderr).
int http_server_init(pw_http_server_t *srv, const char *bind_addr, int port,
                     pw_http_handler_t handler, void *user_data);

// Services ready sockets for up to timeout_ms. Non-blocking throughout, so a
// slow or stalled client cannot delay spill/reconnect maintenance that shares
// this thread. Returns the number of requests dispatched.
int http_server_tick(pw_http_server_t *srv, int timeout_ms);

void http_server_close(pw_http_server_t *srv);
