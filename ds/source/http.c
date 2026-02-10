#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

// Simple HTTP client for DS
// Note: This is a minimal implementation suitable for DS constraints

#define HTTP_BUFFER_SIZE 4096
#define HTTP_TIMEOUT 10

static int socket_fd = -1;

int http_init(void) {
    // Sockets are already available through libc on DS
    return 0;
}

// Parse URL into host and path
static int parse_url(const char *url, char *host, int *port, char *path) {
    // Simple URL parser: http://host:port/path or http://host/path
    const char *start = url;
    
    // Skip protocol
    if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        // HTTPS not supported on DS
        return -1;
    }
    
    // Extract host and optional port
    char *colon = strchr(start, ':');
    char *slash = strchr(start, '/');
    
    int host_len;
    if (colon && (!slash || colon < slash)) {
        // Port specified
        host_len = colon - start;
        strncpy(host, start, host_len);
        host[host_len] = '\0';
        *port = atoi(colon + 1);
    } else if (slash) {
        // No port
        host_len = slash - start;
        strncpy(host, start, host_len);
        host[host_len] = '\0';
        *port = 80;
    } else {
        // No slash
        strcpy(host, start);
        *port = 80;
        strcpy(path, "/");
        return 0;
    }
    
    // Extract path
    if (slash) {
        strcpy(path, slash);
    } else {
        strcpy(path, "/");
    }
    
    return 0;
}

HttpResponse http_request(
    const char *url,
    HttpMethod method,
    const char *api_key,
    const uint8_t *body,
    size_t body_size
) {
    HttpResponse response = {0};
    char host[256] = {0};
    char path[512] = {0};
    int port = 80;
    
    // Parse URL
    if (parse_url(url, host, &port, path) != 0) {
        response.success = 0;
        return response;
    }
    
    // Resolve host
    struct hostent *he = gethostbyname(host);
    if (!he) {
        printf("DNS lookup failed for %s\n", host);
        response.success = 0;
        return response;
    }
    
    // Create socket
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        printf("Socket creation failed\n");
        response.success = 0;
        return response;
    }
    
    // Connect to host
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr = *(struct in_addr*)he->h_addr_list[0];
    
    if (connect(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("Connection failed to %s:%d\n", host, port);
        close(socket_fd);
        socket_fd = -1;
        response.success = 0;
        return response;
    }
    
    // Build HTTP request
    char request[HTTP_BUFFER_SIZE];
    const char *method_str = (method == HTTP_GET) ? "GET" : 
                             (method == HTTP_POST) ? "POST" : "PUT";
    
    snprintf(request, sizeof(request),
        "%s %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: NDSSyncClient/1.0\r\n"
        "X-API-Key: %s\r\n",
        method_str, path, host, api_key);
    
    if (body && body_size > 0) {
        sprintf(request + strlen(request),
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %d\r\n", (int)body_size);
    }
    
    strcat(request, "Connection: close\r\n\r\n");
    
    // Send request headers
    if (send(socket_fd, request, strlen(request), 0) < 0) {
        printf("Failed to send request\n");
        close(socket_fd);
        socket_fd = -1;
        response.success = 0;
        return response;
    }
    
    // Send body if present
    if (body && body_size > 0) {
        if (send(socket_fd, body, body_size, 0) < 0) {
            printf("Failed to send body\n");
            close(socket_fd);
            socket_fd = -1;
            response.success = 0;
            return response;
        }
    }
    
    // Read response headers
    char header_buf[HTTP_BUFFER_SIZE];
    int header_len = recv(socket_fd, header_buf, sizeof(header_buf) - 1, 0);
    if (header_len < 0) {
        printf("Failed to receive response\n");
        close(socket_fd);
        socket_fd = -1;
        response.success = 0;
        return response;
    }
    
    header_buf[header_len] = '\0';
    
    // Parse status code
    sscanf(header_buf, "HTTP/%*d.%*d %d", &response.status_code);
    
    // Find body start (after blank line)
    char *body_start = strstr(header_buf, "\r\n\r\n");
    if (!body_start) {
        body_start = strstr(header_buf, "\n\n");
        if (body_start) body_start += 2;
    } else {
        body_start += 4;
    }
    
    // Calculate body size from header
    int body_offset = body_start - header_buf;
    response.body_size = header_len - body_offset;
    
    if (response.body_size > 0) {
        response.body = malloc(response.body_size);
        memcpy(response.body, body_start, response.body_size);
    }
    
    // Read remaining response body
    // (For now, we're keeping it simple with first chunk)
    
    close(socket_fd);
    socket_fd = -1;
    
    response.success = (response.status_code >= 200 && response.status_code < 300);
    return response;
}

void http_response_free(HttpResponse *response) {
    if (response->body) {
        free(response->body);
        response->body = NULL;
    }
}

void http_cleanup(void) {
    if (socket_fd >= 0) {
        close(socket_fd);
        socket_fd = -1;
    }
}
