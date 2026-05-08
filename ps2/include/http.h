#ifndef PS2SYNC_HTTP_H
#define PS2SYNC_HTTP_H

#include "common.h"

#include <stdio.h>

/*
 * Tiny socket-based HTTP/1.0 client.  Same shape as the NDS client's
 * http.c — no keep-alive, single connection per request.
 *
 * URL handling: we accept ``http://host:port/path`` and split it
 * ourselves; no URL library needed.
 */

typedef int (*HttpProgressFn)(uint64_t downloaded, uint64_t total);

typedef struct {
    const char *server_url;     /* "http://host:port" — no trailing slash */
    const char *api_key;
    const char *path;           /* "/api/v1/..." */
    const char *method;         /* "GET" / "POST" */
    /* Optional Range header: byte offset to start at; 0 = none. */
    uint64_t    range_start;
    /* Optional request body. */
    const uint8_t *body;
    uint32_t       body_len;
    const char    *body_content_type;   /* "application/json" etc. */
} HttpRequest;

typedef struct {
    int      status;            /* HTTP status code */
    uint64_t content_length;    /* 0 if unknown */
} HttpResponseInfo;

/*
 * Fixed-buffer GET: writes body bytes into ``out`` (capped at out_size).
 * Returns body length on success, negative on failure.  status_out
 * receives the HTTP status code (0 if unreachable).
 */
int  http_get_buf(const HttpRequest *req,
                  uint8_t *out, uint32_t out_size,
                  int *status_out);

/*
 * Streaming GET: writes body bytes to a stdio FILE.  Caller should
 * setvbuf() with a fat (256 KB+) buffer before calling so the writes
 * batch instead of going through fileXio one-recv-at-a-time.
 *
 * Optional progress callback fires every ~recv chunk; returning
 * non-zero aborts.  Returns 0 on success, 1 on user-abort, negative
 * on error.
 */
int  http_get_stream(const HttpRequest *req,
                     FILE *out_fp,
                     HttpProgressFn progress,
                     HttpResponseInfo *info_out);

#endif /* PS2SYNC_HTTP_H */
