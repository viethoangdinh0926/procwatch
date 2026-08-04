// Minimal non-blocking HTTP/1.1 server for the OTLP receiver.
//
// Only what OTLP/HTTP needs: POST with Content-Length, optional gzip, and a
// handful of fixed paths. No chunked encoding (the OTLP spec does not
// require producers to use it and no official exporter does), no TLS, no
// static files.
//
// The loop is non-blocking so a client that opens a connection and stalls
// cannot delay other work on this thread (reconnect/spill maintenance).

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <zlib.h>

#include "../../include/agent/http_server.h"

#define CONN_INITIAL_CAP 8192
#define HEADER_MAX 16384

static void conn_reset(pw_conn_t *c) {
    if (c->fd >= 0) close(c->fd);
    free(c->buf);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int http_server_init(pw_http_server_t *srv, const char *bind_addr, int port,
                     pw_http_handler_t handler, void *user_data) {
    memset(srv, 0, sizeof *srv);
    for (int i = 0; i < PW_HTTP_MAX_CONNS; ++i) srv->conns[i].fd = -1;
    srv->handler = handler;
    srv->user_data = user_data;
    srv->max_body = 16u * 1024u * 1024u;
    srv->idle_timeout_sec = 30;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket failed: %s\n", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (!bind_addr || !*bind_addr || strcmp(bind_addr, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid bind address: %s\n", bind_addr);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        fprintf(stderr, "bind %s:%d failed: %s\n",
                bind_addr ? bind_addr : "0.0.0.0", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 64) != 0) {
        fprintf(stderr, "listen failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (set_nonblock(fd) != 0) {
        fprintf(stderr, "set_nonblock failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    srv->listen_fd = fd;
    return 0;
}

void http_server_close(pw_http_server_t *srv) {
    for (int i = 0; i < PW_HTTP_MAX_CONNS; ++i) {
        if (srv->conns[i].fd >= 0) conn_reset(&srv->conns[i]);
    }
    if (srv->listen_fd >= 0) close(srv->listen_fd);
    srv->listen_fd = -1;
}

// ------------------------ Header parsing ------------------------

static int ci_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

static pw_signal_t signal_for_path(const char *path) {
    if (strcmp(path, "/v1/traces") == 0) return PW_SIGNAL_TRACES;
    if (strcmp(path, "/v1/metrics") == 0) return PW_SIGNAL_METRICS;
    if (strcmp(path, "/v1/logs") == 0) return PW_SIGNAL_LOGS;
    if (strcmp(path, "/v1/procmetrics") == 0) return PW_SIGNAL_PROCMETRICS;
    return PW_SIGNAL_UNKNOWN;
}

// Parses the request line and headers found in c->buf. Returns 1 when the
// header block is complete, 0 if more bytes are needed, -1 on a malformed
// request.
static int parse_headers(pw_conn_t *c) {
    char *blank = memmem(c->buf, c->len, "\r\n\r\n", 4);
    size_t sep = 4;
    if (!blank) {
        blank = memmem(c->buf, c->len, "\n\n", 2);
        sep = 2;
    }
    if (!blank) {
        if (c->len > HEADER_MAX) return -1;
        return 0;
    }

    size_t header_len = (size_t)((uint8_t *)blank - c->buf);
    char header[HEADER_MAX + 1];
    if (header_len > HEADER_MAX) return -1;
    memcpy(header, c->buf, header_len);
    header[header_len] = '\0';

    // Request line: METHOD SP PATH SP VERSION
    char *line_end = strstr(header, "\r\n");
    if (!line_end) line_end = strchr(header, '\n');
    size_t line_len = line_end ? (size_t)(line_end - header) : header_len;

    char line[512];
    if (line_len >= sizeof line) return -1;
    memcpy(line, header, line_len);
    line[line_len] = '\0';

    char *sp1 = strchr(line, ' ');
    if (!sp1) return -1;
    *sp1 = '\0';
    char *path = sp1 + 1;
    char *sp2 = strchr(path, ' ');
    if (sp2) *sp2 = '\0';

    // Strip any query string; OTLP does not use one but proxies may add it.
    char *q = strchr(path, '?');
    if (q) *q = '\0';

    if (strcmp(line, "POST") != 0) {
        // Recorded so the caller can reply 405 rather than hanging.
        snprintf(c->path, sizeof c->path, "%s", path);
        c->body_offset = header_len + sep;
        c->content_length = 0;
        c->headers_done = 1;
        c->keep_alive = 0;
        return 1;
    }
    snprintf(c->path, sizeof c->path, "%s", path);

    c->content_length = 0;
    c->gzip = 0;
    c->keep_alive = 1; // HTTP/1.1 default

    char *cursor = line_end ? (char *)(header + line_len) : NULL;
    if (cursor) {
        while (*cursor == '\r' || *cursor == '\n') ++cursor;
        while (*cursor) {
            char *eol = strstr(cursor, "\r\n");
            if (!eol) eol = strchr(cursor, '\n');
            size_t n = eol ? (size_t)(eol - cursor) : strlen(cursor);

            if (ci_starts_with(cursor, "content-length:")) {
                const char *v = skip_ws(cursor + strlen("content-length:"));
                c->content_length = (size_t)strtoull(v, NULL, 10);
            } else if (ci_starts_with(cursor, "content-encoding:")) {
                const char *v = skip_ws(cursor + strlen("content-encoding:"));
                if (ci_starts_with(v, "gzip") || ci_starts_with(v, "deflate")) c->gzip = 1;
            } else if (ci_starts_with(cursor, "connection:")) {
                const char *v = skip_ws(cursor + strlen("connection:"));
                if (ci_starts_with(v, "close")) c->keep_alive = 0;
            }

            if (!eol) break;
            cursor += n;
            while (*cursor == '\r' || *cursor == '\n') ++cursor;
        }
    }

    c->body_offset = header_len + sep;
    c->headers_done = 1;
    return 1;
}

// ------------------------ gzip ------------------------

// Window bits 15+32 lets zlib auto-detect gzip vs zlib framing, which covers
// both Content-Encoding: gzip and deflate without branching.
static int gzip_inflate(const uint8_t *in, size_t in_len, size_t max_out,
                        uint8_t **out, size_t *out_len) {
    z_stream strm;
    memset(&strm, 0, sizeof strm);
    if (inflateInit2(&strm, 15 + 32) != Z_OK) return -1;

    size_t cap = in_len * 4 + 1024;
    if (cap > max_out) cap = max_out;
    uint8_t *buf = malloc(cap);
    if (!buf) { inflateEnd(&strm); return -1; }

    strm.next_in = (Bytef *)in;
    strm.avail_in = (uInt)in_len;
    strm.next_out = buf;
    strm.avail_out = (uInt)cap;

    int rc;
    for (;;) {
        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) { free(buf); inflateEnd(&strm); return -1; }
        if (strm.avail_out == 0) {
            if (cap >= max_out) { free(buf); inflateEnd(&strm); return -1; }
            size_t newcap = cap * 2;
            if (newcap > max_out) newcap = max_out;
            uint8_t *tmp = realloc(buf, newcap);
            if (!tmp) { free(buf); inflateEnd(&strm); return -1; }
            strm.next_out = tmp + cap;
            strm.avail_out = (uInt)(newcap - cap);
            buf = tmp;
            cap = newcap;
        }
    }

    *out_len = cap - strm.avail_out;
    *out = buf;
    inflateEnd(&strm);
    return 0;
}

// ------------------------ Responses ------------------------

static void send_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EINTR)) continue;
        // A blocked or dead peer is not worth stalling the loop for; the
        // response body is empty, so a partial write means the client is gone.
        break;
    }
}

static void respond(pw_conn_t *c, int status, const char *reason) {
    char head[256];
    // A successful OTLP response carries an empty ExportTraceServiceResponse,
    // whose only field (partial_success) must be unset. That serialises to
    // zero bytes, so Content-Length: 0 is the correct full response.
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/x-protobuf\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     status, reason, c->keep_alive ? "keep-alive" : "close");
    if (n > 0) send_all(c->fd, head, (size_t)n);
}

// ------------------------ Connection servicing ------------------------

static int conn_ensure_cap(pw_conn_t *c, size_t needed) {
    if (c->cap >= needed) return 0;
    size_t cap = c->cap ? c->cap : CONN_INITIAL_CAP;
    while (cap < needed) cap *= 2;
    uint8_t *tmp = realloc(c->buf, cap);
    if (!tmp) return -1;
    c->buf = tmp;
    c->cap = cap;
    return 0;
}

// Returns 1 if a request was dispatched, 0 otherwise. Closes the connection
// on error or when the peer is done.
static int conn_service(pw_http_server_t *srv, pw_conn_t *c) {
    if (conn_ensure_cap(c, c->len + 4096) != 0) { conn_reset(c); return 0; }

    ssize_t n = recv(c->fd, c->buf + c->len, c->cap - c->len, 0);
    if (n == 0) { conn_reset(c); return 0; }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        conn_reset(c);
        return 0;
    }
    c->len += (size_t)n;
    c->last_active = time(NULL);

    if (!c->headers_done) {
        int rc = parse_headers(c);
        if (rc < 0) { conn_reset(c); return 0; }
        if (rc == 0) return 0;

        if (c->content_length > srv->max_body) {
            c->keep_alive = 0;
            respond(c, 413, "Payload Too Large");
            ++srv->rejected;
            conn_reset(c);
            return 0;
        }
    }

    size_t have = c->len - c->body_offset;
    if (have < c->content_length) {
        if (conn_ensure_cap(c, c->body_offset + c->content_length) != 0) {
            conn_reset(c);
        }
        return 0;
    }

    int dispatched = 0;
    pw_signal_t sig = signal_for_path(c->path);
    if (sig == PW_SIGNAL_UNKNOWN) {
        respond(c, 404, "Not Found");
        ++srv->rejected;
    } else {
        const uint8_t *body = c->buf + c->body_offset;
        size_t body_len = c->content_length;
        uint8_t *inflated = NULL;

        if (c->gzip && body_len) {
            size_t ilen = 0;
            if (gzip_inflate(body, body_len, srv->max_body, &inflated, &ilen) == 0) {
                body = inflated;
                body_len = ilen;
            } else {
                respond(c, 400, "Bad Request");
                ++srv->rejected;
                free(inflated);
                goto after_dispatch;
            }
        }

        pw_http_request_t req = {
            .signal = sig,
            .path = c->path,
            .body = body,
            .body_len = body_len,
        };
        int hrc = srv->handler ? srv->handler(&req, srv->user_data) : 0;
        respond(c, hrc == 0 ? 200 : 400, hrc == 0 ? "OK" : "Bad Request");
        free(inflated);
        ++srv->requests;
        dispatched = 1;
    }

after_dispatch:
    if (!c->keep_alive) {
        conn_reset(c);
        return dispatched;
    }

    // Carry over any pipelined bytes that belong to the next request.
    size_t consumed = c->body_offset + c->content_length;
    size_t leftover = c->len - consumed;
    if (leftover) memmove(c->buf, c->buf + consumed, leftover);
    c->len = leftover;
    c->headers_done = 0;
    c->body_offset = 0;
    c->content_length = 0;
    c->gzip = 0;
    c->path[0] = '\0';
    return dispatched;
}

static void accept_new(pw_http_server_t *srv) {
    for (;;) {
        int fd = accept(srv->listen_fd, NULL, NULL);
        if (fd < 0) return;

        int slot = -1;
        for (int i = 0; i < PW_HTTP_MAX_CONNS; ++i) {
            if (srv->conns[i].fd < 0) { slot = i; break; }
        }
        if (slot < 0) { close(fd); ++srv->rejected; continue; }

        if (set_nonblock(fd) != 0) { close(fd); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        pw_conn_t *c = &srv->conns[slot];
        memset(c, 0, sizeof *c);
        c->fd = fd;
        c->keep_alive = 1;
        c->last_active = time(NULL);
    }
}

int http_server_tick(pw_http_server_t *srv, int timeout_ms) {
    struct pollfd fds[PW_HTTP_MAX_CONNS + 1];
    pw_conn_t *owners[PW_HTTP_MAX_CONNS + 1];
    nfds_t nfds = 0;

    fds[nfds].fd = srv->listen_fd;
    fds[nfds].events = POLLIN;
    fds[nfds].revents = 0;
    owners[nfds] = NULL;
    ++nfds;

    for (int i = 0; i < PW_HTTP_MAX_CONNS; ++i) {
        if (srv->conns[i].fd < 0) continue;
        fds[nfds].fd = srv->conns[i].fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        owners[nfds] = &srv->conns[i];
        ++nfds;
    }

    int rc = poll(fds, nfds, timeout_ms);
    if (rc < 0) return 0;

    int dispatched = 0;
    if (rc > 0) {
        if (fds[0].revents & POLLIN) accept_new(srv);
        for (nfds_t i = 1; i < nfds; ++i) {
            pw_conn_t *c = owners[i];
            if (c->fd < 0) continue;
            if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                conn_reset(c);
                continue;
            }
            if (fds[i].revents & POLLIN) dispatched += conn_service(srv, c);
        }
    }

    // Reap idle connections so a pod that dies mid-request cannot leak a slot.
    time_t now = time(NULL);
    for (int i = 0; i < PW_HTTP_MAX_CONNS; ++i) {
        pw_conn_t *c = &srv->conns[i];
        if (c->fd < 0) continue;
        if (now - c->last_active > srv->idle_timeout_sec) conn_reset(c);
    }
    return dispatched;
}
