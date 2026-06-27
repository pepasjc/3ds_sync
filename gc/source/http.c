/*
 * http.c — minimal HTTP/1.0 client over libogc's net_* BSD sockets (BBA).
 *
 * Same protocol shape as the PS2/NDS clients.  No keep-alive, no chunked
 * encoding, no TLS.  Host must be a dotted LAN IP (no DNS).
 *
 * NOTE: GameCube/PPC is big-endian, so the socket address is built with
 * htonl/htons rather than the byte-wise little-endian trick the PS2 used.
 */

#include "http.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <gccore.h>
#include <network.h>

#ifndef IPPROTO_IP
#define IPPROTO_IP 0
#endif

#define RECV_BUF_SIZE 8192

static int parse_url(const char *url, char *host_out, size_t host_size,
                     int *port_out, char *path_out, size_t path_size)
{
    const char *p = url;
    if (strncmp(p, "http://", 7) != 0) return -1;
    p += 7;

    const char *host_start = p;
    while (*p && *p != ':' && *p != '/') p++;
    size_t host_len = p - host_start;
    if (host_len == 0 || host_len >= host_size) return -1;
    memcpy(host_out, host_start, host_len);
    host_out[host_len] = '\0';

    int port = 80;
    if (*p == ':') {
        p++;
        port = atoi(p);
        while (*p && *p != '/') p++;
    }
    *port_out = port;

    if (*p == '\0') {
        if (path_size > 1) { path_out[0] = '/'; path_out[1] = '\0'; }
    } else {
        size_t plen = strlen(p);
        if (plen >= path_size) return -1;
        strcpy(path_out, p);
    }
    return 0;
}

static int parse_dotted_ip(const char *host, struct in_addr *out) {
    unsigned a, b, c, d;
    char extra;
    if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    out->s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
    return 0;
}

static int connect_to(const char *host, int port) {
    struct in_addr ip;
    if (parse_dotted_ip(host, &ip) != 0) return -2;

    int fd = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return -1;

    /* Bigger receive window — single-stream LAN throughput is window-bound. */
    int rcvbuf = 128 * 1024;
    net_setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u16)port);
    addr.sin_addr.s_addr = ip.s_addr;

    if (net_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        net_close(fd);
        return -3;
    }
    return fd;
}

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t remaining = len;
    while (remaining) {
        int n = net_send(fd, p, remaining, 0);
        if (n <= 0) return -1;
        p         += n;
        remaining -= n;
    }
    return 0;
}

static int build_request(char *out, size_t out_size,
                         const HttpRequest *req,
                         const char *host, int port,
                         const char *path)
{
    int n = snprintf(out, out_size,
        "%s %s HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "X-API-Key: %s\r\n"
        "User-Agent: gcsync/" APP_VERSION "\r\n"
        "Connection: close\r\n",
        req->method ? req->method : "GET",
        path, host, port,
        req->api_key ? req->api_key : "");
    if (n < 0 || (size_t)n >= out_size) return -1;

    if (req->range_start > 0) {
        n += snprintf(out + n, out_size - n,
                      "Range: bytes=%llu-\r\n",
                      (unsigned long long)req->range_start);
        if ((size_t)n >= out_size) return -1;
    }
    if (req->body_len > 0) {
        n += snprintf(out + n, out_size - n,
                      "Content-Type: %s\r\n"
                      "Content-Length: %lu\r\n",
                      req->body_content_type ? req->body_content_type
                                             : "application/octet-stream",
                      (unsigned long)req->body_len);
        if ((size_t)n >= out_size) return -1;
    }
    n += snprintf(out + n, out_size - n, "\r\n");
    return n;
}

static int read_headers(int fd, char *buf, size_t buf_size, size_t *total_out) {
    size_t total = 0;
    if (total_out) *total_out = 0;
    while (total + 1 < buf_size) {
        int n = net_recv(fd, buf + total, buf_size - 1 - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
        buf[total] = '\0';
        char *eoh = strstr(buf, "\r\n\r\n");
        if (eoh) {
            size_t header_len = (size_t)((eoh - buf) + 4);
            if (total_out) *total_out = total;
            return (int)header_len;
        }
    }
    return -1;
}

static int parse_status(const char *headers) {
    if (strncmp(headers, "HTTP/", 5) != 0) return 0;
    const char *p = strchr(headers, ' ');
    if (!p) return 0;
    return atoi(p + 1);
}

static uint64_t parse_content_length(const char *headers, size_t header_len) {
    const char *needle = "content-length:";
    size_t nlen = strlen(needle);
    for (size_t i = 0; i + nlen < header_len; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)headers[i + j]) != needle[j]) { match = false; break; }
        }
        if (match) {
            const char *p = headers + i + nlen;
            while (*p == ' ' || *p == '\t') p++;
            return strtoull(p, NULL, 10);
        }
    }
    return 0;
}

int http_get_buf(const HttpRequest *req,
                 uint8_t *out, uint32_t out_size,
                 int *status_out)
{
    if (status_out) *status_out = 0;

    char host[64];
    int  port;
    char path[1024];
    if (parse_url(req->server_url, host, sizeof(host), &port, path, sizeof(path)) != 0)
        return -1;
    snprintf(path, sizeof(path), "%s", req->path);

    int fd = connect_to(host, port);
    if (fd < 0) return -2;

    char header_buf[1024];
    int  header_len = build_request(header_buf, sizeof(header_buf), req, host, port, path);
    if (header_len < 0) { net_close(fd); return -3; }

    if (send_all(fd, header_buf, header_len) < 0) { net_close(fd); return -4; }
    if (req->body_len > 0 && send_all(fd, req->body, req->body_len) < 0) {
        net_close(fd); return -4;
    }

    char rbuf[RECV_BUF_SIZE];
    size_t total_read = 0;
    int  hlen = read_headers(fd, rbuf, sizeof(rbuf), &total_read);
    if (hlen < 0) { net_close(fd); return -5; }

    int status = parse_status(rbuf);
    if (status_out) *status_out = status;

    uint32_t cap = out_size > 0 ? out_size - 1 : 0;

    uint64_t content_length = parse_content_length(rbuf, (size_t)hlen);
    if (content_length > (uint64_t)cap) { net_close(fd); return -6; }

    size_t body_in_rbuf = (total_read > (size_t)hlen) ? total_read - (size_t)hlen : 0;

    uint32_t written = 0;
    if (body_in_rbuf > 0 && cap > 0) {
        uint32_t take = (body_in_rbuf < cap) ? (uint32_t)body_in_rbuf : cap;
        memcpy(out, rbuf + hlen, take);
        written = take;
    }
    while (written < cap) {
        int n = net_recv(fd, out + written, cap - written, 0);
        if (n <= 0) break;
        written += n;
    }
    if (out_size > 0) out[written] = '\0';

    net_close(fd);
    return (int)written;
}

static int file_stream_writer(const void *data, uint32_t len, void *user) {
    FILE *fp = (FILE *)user;
    if (!fp) return -1;
    return fwrite(data, 1, len, fp) == len ? 0 : -1;
}

int http_get_stream_cb(const HttpRequest *req,
                       HttpStreamBeginFn begin,
                       HttpWriteFn writer,
                       void *user,
                       HttpProgressFn progress,
                       HttpResponseInfo *info_out)
{
    if (info_out) { info_out->status = 0; info_out->content_length = 0; }
    if (!writer) return -1;

    char host[64];
    int  port;
    char path[1024];
    if (parse_url(req->server_url, host, sizeof(host), &port, path, sizeof(path)) != 0)
        return -1;
    snprintf(path, sizeof(path), "%s", req->path);

    int fd = connect_to(host, port);
    if (fd < 0) return -2;

    char header_buf[1024];
    int  header_len = build_request(header_buf, sizeof(header_buf), req, host, port, path);
    if (header_len < 0) { net_close(fd); return -3; }

    if (send_all(fd, header_buf, header_len) < 0) { net_close(fd); return -4; }
    if (req->body_len > 0 && send_all(fd, req->body, req->body_len) < 0) {
        net_close(fd); return -4;
    }

    char rbuf[RECV_BUF_SIZE];
    size_t total_read = 0;
    int  hlen = read_headers(fd, rbuf, sizeof(rbuf), &total_read);
    if (hlen < 0) { net_close(fd); return -5; }

    int status = parse_status(rbuf);
    uint64_t content_length = parse_content_length(rbuf, (size_t)hlen);
    if (info_out) { info_out->status = status; info_out->content_length = content_length; }

    if (status < 200 || status >= 300) { net_close(fd); return -6; }

    if (begin && begin(content_length, user) != 0) { net_close(fd); return -9; }

    uint64_t total_done   = 0;
    size_t   in_rbuf_body = (total_read > (size_t)hlen) ? total_read - (size_t)hlen : 0;
    if (in_rbuf_body > 0) {
        if (writer(rbuf + hlen, (uint32_t)in_rbuf_body, user) != 0) { net_close(fd); return -7; }
        total_done += in_rbuf_body;
    }

    static char chunk[65536];
    while (1) {
        int n = net_recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0) break;
        if (n < 0)  { net_close(fd); return -8; }
        if (writer(chunk, (uint32_t)n, user) != 0) { net_close(fd); return -7; }
        total_done += (uint64_t)n;
        if (progress && progress(total_done, content_length) != 0) { net_close(fd); return 1; }
    }

    net_close(fd);
    return 0;
}

int http_get_stream(const HttpRequest *req,
                    FILE *out_fp,
                    HttpProgressFn progress,
                    HttpResponseInfo *info_out)
{
    int rc = http_get_stream_cb(req, NULL, file_stream_writer, out_fp, progress, info_out);
    if (out_fp) fflush(out_fp);
    return rc;
}
