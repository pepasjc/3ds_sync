#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <stddef.h>

// HTTP method types
typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_PUT
} HttpMethod;

// HTTP response
typedef struct {
    int status_code;
    uint8_t *body;
    size_t body_size;
    int success;
} HttpResponse;

// HTTP client initialization
int http_init(void);

// Make HTTP request
HttpResponse http_request(
    const char *url,
    HttpMethod method,
    const char *api_key,
    const uint8_t *body,
    size_t body_size
);

// Free response body
void http_response_free(HttpResponse *response);

// Cleanup HTTP
void http_cleanup(void);

#endif
