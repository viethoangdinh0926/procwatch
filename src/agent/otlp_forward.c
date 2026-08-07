// Best-effort OTLP/HTTP forwarder for optional Tempo / Prometheus dual-export.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>

#include "../../include/agent/otlp_forward.h"

#define PW_FWD_TIMEOUT_SEC 2

static int parse_http_url(const char *url, char *host, size_t host_cap,
                          int *port, char *path, size_t path_cap) {
    if (!url || !*url) return -1;
    const char *p = url;
    if (strncmp(p, "http://", 7) != 0) return -1;
    p += 7;

    const char *slash = strchr(p, '/');
    const char *colon = NULL;
    for (const char *q = p; q != slash && *q; ++q) {
        if (*q == ':') { colon = q; break; }
    }

    size_t host_len;
    if (colon) {
        host_len = (size_t)(colon - p);
        *port = atoi(colon + 1);
    } else if (slash) {
        host_len = (size_t)(slash - p);
        *port = 80;
    } else {
        host_len = strlen(p);
        *port = 80;
    }
    if (host_len == 0 || host_len >= host_cap) return -1;
    memcpy(host, p, host_len);
    host[host_len] = '\0';
    if (*port <= 0 || *port > 65535) return -1;

    if (slash && slash[1]) {
        snprintf(path, path_cap, "%s", slash);
    } else {
        snprintf(path, path_cap, "/");
    }
    return 0;
}

int otlp_forward_url_valid(const char *url) {
    char host[256], path[512];
    int port = 0;
    return parse_http_url(url, host, sizeof host, &port, path, sizeof path) == 0;
}

static int set_timeouts(int fd) {
    struct timeval tv;
    tv.tv_sec = PW_FWD_TIMEOUT_SEC;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0) return -1;
    return 0;
}

static int connect_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;

    int rc = connect(fd, addr, addrlen);
    if (rc == 0) {
        fcntl(fd, F_SETFL, flags);
        return 0;
    }
    if (errno != EINPROGRESS) {
        fcntl(fd, F_SETFL, flags);
        return -1;
    }

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv;
    tv.tv_sec = PW_FWD_TIMEOUT_SEC;
    tv.tv_usec = 0;
    rc = select(fd + 1, NULL, &wset, NULL, &tv);
    if (rc <= 0) {
        fcntl(fd, F_SETFL, flags);
        return -1;
    }

    int soerr = 0;
    socklen_t slen = sizeof soerr;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 || soerr != 0) {
        fcntl(fd, F_SETFL, flags);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    return 0;
}

static int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

int otlp_forward_post(const char *url, const uint8_t *body, size_t body_len) {
    if (!otlp_forward_url_valid(url)) return -1;
    if (!body && body_len) return -1;

    char host[256], path[512];
    int port = 80;
    if (parse_http_url(url, host, sizeof host, &port, path, sizeof path) != 0)
        return -1;

    char hdr[768];
    int hlen = snprintf(hdr, sizeof hdr,
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/x-protobuf\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port, body_len);
    if (hlen <= 0 || (size_t)hlen >= sizeof hdr) return -1;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portbuf[16];
    snprintf(portbuf, sizeof portbuf, "%d", port);
    if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (set_timeouts(fd) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (connect_timeout(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    if (send_all(fd, hdr, (size_t)hlen) != 0) {
        close(fd);
        return -1;
    }
    if (body_len && send_all(fd, body, body_len) != 0) {
        close(fd);
        return -1;
    }

    char resp[256];
    ssize_t n = recv(fd, resp, sizeof resp - 1, 0);
    close(fd);
    if (n <= 0) return -1;
    resp[n] = '\0';
    char *sp = strchr(resp, ' ');
    if (!sp) return -1;
    int code = atoi(sp + 1);
    return (code >= 200 && code < 300) ? 0 : -1;
}
