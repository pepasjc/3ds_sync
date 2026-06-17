/*
 * http.c — minimal HTTP/1.0 client over BSD sockets.
 *
 * Same approach as the NDS client (which has working sockets via
 * dswifi).  PS2 sockets come from ps2ip + ps2ips so the standard
 * <sys/socket.h> + <netinet/in.h> headers Just Work once netman is up.
 *
 * No keep-alive, no chunked transfer-encoding, no TLS.  All endpoints
 * the Save Sync server exposes are plain HTTP/1.0 friendly, so this
 * stays small.
 */

#include "http.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/* ps2ip's lwIP doesn't ship gethostbyname out of the box, so we limit
 * the URL parser to host = dotted IP for now.  Server URL is an LAN
 * address in practice; DNS can come later. */

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

/* Hand-rolled dotted-IPv4 parser — PS2SDK's libcglue inet_addr() routes
 * through a function pointer (_libcglue_fdman_inet_ops) that ps2ip never
 * populates, so inet_addr() always returns 0 and connect() ends up
 * pointing at 0.0.0.0. */
static int parse_dotted_ip(const char *host, uint32_t *out_be) {
    unsigned a, b, c, d;
    char extra;
    if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    /* sin_addr.s_addr is network byte order; build it byte-wise so we
     * don't rely on htonl which on some lwIP builds is also a stub. */
    *out_be = ((uint32_t)a) | ((uint32_t)b << 8) |
              ((uint32_t)c << 16) | ((uint32_t)d << 24);
    return 0;
}

static int connect_to(const char *host, int port) {
    uint32_t ip_be = 0;
    if (parse_dotted_ip(host, &ip_be) != 0) return -2;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* Bump the lwIP receive buffer so the advertised TCP window grows
     * past its 8-16 KB default.  Single-stream LAN throughput on PS2
     * is window-bound — without this we cap at ~95 KB/s no matter how
     * fast the server can produce data, because the sender stalls
     * waiting for ACKs every ~16 KB of round-trip. */
    int rcvbuf = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    /* htons via byte-build because PS2SDK's libcglue htons may fall
     * through the same uninitialised fdman as inet_addr. */
    addr.sin_port        = (uint16_t)(((port & 0xff) << 8) | ((port >> 8) & 0xff));
    addr.sin_addr.s_addr = ip_be;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -3;
    }
    return fd;
}

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t remaining = len;
    while (remaining) {
        int n = send(fd, p, remaining, 0);
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
        "User-Agent: ps2sync/" APP_VERSION "\r\n"
        "Connection: close\r\n",
        req->method ? req->method : "GET",
        path,
        host, port,
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
                      req->body_content_type ? req->body_content_type : "application/octet-stream",
                      (unsigned long)req->body_len);
        if ((size_t)n >= out_size) return -1;
    }

    n += snprintf(out + n, out_size - n, "\r\n");
    return n;
}

/* Read until end-of-headers (\r\n\r\n) or buffer full.  Returns header
 * length (which is also the offset where body bytes begin in buf) and
 * stores the actual byte count read in total_out. */
static int read_headers(int fd, char *buf, size_t buf_size, size_t *total_out) {
    size_t total = 0;
    if (total_out) *total_out = 0;
    while (total + 1 < buf_size) {
        int n = recv(fd, buf + total, buf_size - 1 - total, 0);
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
    /* Case-insensitive header search.  Headers terminate at header_len. */
    const char *needle = "content-length:";
    size_t nlen = strlen(needle);
    for (size_t i = 0; i + nlen < header_len; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)headers[i + j]) != needle[j]) {
                match = false;
                break;
            }
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
    if (parse_url(req->server_url, host, sizeof(host), &port,
                  /* dummy */ path, sizeof(path)) != 0) {
        return -1;
    }
    /* parse_url put the URL's own path into ``path``, but we want
     * req->path for the actual request. */
    snprintf(path, sizeof(path), "%s", req->path);

    int fd = connect_to(host, port);
    if (fd < 0) return -2;

    char header_buf[1024];
    int  header_len = build_request(header_buf, sizeof(header_buf),
                                    req, host, port, path);
    if (header_len < 0) { close(fd); return -3; }

    if (send_all(fd, header_buf, header_len) < 0) {
        close(fd);
        return -4;
    }
    if (req->body_len > 0) {
        if (send_all(fd, req->body, req->body_len) < 0) {
            close(fd);
            return -4;
        }
    }

    char rbuf[RECV_BUF_SIZE];
    size_t total_read = 0;
    int  hlen = read_headers(fd, rbuf, sizeof(rbuf), &total_read);
    if (hlen < 0) { close(fd); return -5; }

    int status = parse_status(rbuf);
    if (status_out) *status_out = status;

    uint32_t cap = out_size > 0 ? out_size - 1 : 0;

    /* If the server advertised a body larger than the caller's buffer, fail
     * loudly rather than silently returning a truncated payload (callers parse
     * JSON / bundles from this and a cut-off body corrupts them). */
    uint64_t content_length = parse_content_length(rbuf, (size_t)hlen);
    if (content_length > (uint64_t)cap) {
        close(fd);
        return -6;
    }

    /* Body bytes already in rbuf after the headers.  Use the byte count
     * from recv(), not strlen(), so binary data and embedded NULs are safe. */
    size_t body_in_rbuf = (total_read > (size_t)hlen)
                        ? total_read - (size_t)hlen
                        : 0;

    uint32_t written = 0;
    if (body_in_rbuf > 0 && cap > 0) {
        uint32_t take = (body_in_rbuf < cap) ? (uint32_t)body_in_rbuf : cap;
        memcpy(out, rbuf + hlen, take);
        written = take;
    }

    while (written < cap) {
        int n = recv(fd, out + written, cap - written, 0);
        if (n <= 0) break;
        written += n;
    }
    if (out_size > 0) out[written] = '\0';

    close(fd);
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
    if (info_out) {
        info_out->status         = 0;
        info_out->content_length = 0;
    }
    if (!writer) return -1;

    char host[64];
    int  port;
    char path[1024];
    if (parse_url(req->server_url, host, sizeof(host), &port,
                  path, sizeof(path)) != 0) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s", req->path);

    int fd = connect_to(host, port);
    if (fd < 0) return -2;

    char header_buf[1024];
    int  header_len = build_request(header_buf, sizeof(header_buf),
                                    req, host, port, path);
    if (header_len < 0) { close(fd); return -3; }

    if (send_all(fd, header_buf, header_len) < 0) { close(fd); return -4; }
    if (req->body_len > 0 &&
        send_all(fd, req->body, req->body_len) < 0)
    {
        close(fd);
        return -4;
    }

    char rbuf[RECV_BUF_SIZE];
    size_t total_read = 0;
    int  hlen = read_headers(fd, rbuf, sizeof(rbuf), &total_read);
    if (hlen < 0) { close(fd); return -5; }

    int status = parse_status(rbuf);
    uint64_t content_length = parse_content_length(rbuf, (size_t)hlen);
    if (info_out) {
        info_out->status         = status;
        info_out->content_length = content_length;
    }

    if (status < 200 || status >= 300) {
        close(fd);
        return -6;
    }

    if (begin && begin(content_length, user) != 0) {
        close(fd);
        return -9;
    }

    /* Body bytes already in rbuf — deliver before continuing recv loop. */
    uint64_t total_done   = 0;
    size_t   in_rbuf_body = (total_read > (size_t)hlen)
                           ? total_read - (size_t)hlen
                           : 0;
    if (in_rbuf_body > 0) {
        if (writer(rbuf + hlen, (uint32_t)in_rbuf_body, user) != 0) {
            close(fd);
            return -7;
        }
        total_done += in_rbuf_body;
    }

    /* Big recv chunks land in caller's setvbuf'd stdio buffer; once
     * the buffer fills newlib emits a single big write() to fileXio.
     * That batching is the difference between ~95 KB/s (default 4 KB
     * stdio buffer = 16 syscalls per recv) and the ~350 KB/s the
     * USB 1.1 + FAT32 stack can sustain in practice. */
    static char chunk[131072];
    while (1) {
        int n = recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0) break;        /* peer closed */
        if (n < 0)  { close(fd); return -8; }
        if (writer(chunk, (uint32_t)n, user) != 0) {
            close(fd);
            return -7;
        }
        total_done += (uint64_t)n;

        if (progress && progress(total_done, content_length) != 0) {
            close(fd);
            return 1;
        }
    }

    close(fd);
    return 0;
}

int http_get_stream(const HttpRequest *req,
                    FILE *out_fp,
                    HttpProgressFn progress,
                    HttpResponseInfo *info_out)
{
    int rc = http_get_stream_cb(req, NULL, file_stream_writer,
                                out_fp, progress, info_out);
    if (out_fp) fflush(out_fp);
    return rc;
}
