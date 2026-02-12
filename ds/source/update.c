#include "update.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fat.h>

// Simple JSON string extraction
static bool json_get_string(const char *json, const char *key, char *out, int out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char *pos = strstr(json, search);
    if (!pos) return false;

    pos += strlen(search);
    while (*pos == ' ' || *pos == '\t') pos++;

    if (strncmp(pos, "null", 4) == 0) {
        out[0] = '\0';
        return false;
    }

    if (*pos != '"') return false;
    pos++;

    int i = 0;
    while (*pos && *pos != '"' && i < out_size - 1) {
        if (*pos == '\\' && *(pos + 1) == '"') {
            pos++;
        }
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return i > 0;
}

// Extract boolean value from JSON
static bool json_get_bool(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char *pos = strstr(json, search);
    if (!pos) return false;

    pos += strlen(search);
    while (*pos == ' ' || *pos == '\t') pos++;

    return strncmp(pos, "true", 4) == 0;
}

// Extract integer value from JSON
static int json_get_int(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char *pos = strstr(json, search);
    if (!pos) return 0;

    pos += strlen(search);
    while (*pos == ' ' || *pos == '\t') pos++;

    return atoi(pos);
}

bool update_check(SyncState *state, UpdateInfo *info) {
    memset(info, 0, sizeof(UpdateInfo));

    // Strip trailing slash from server_url
    char server_url[256];
    strncpy(server_url, state->server_url, sizeof(server_url) - 1);
    server_url[sizeof(server_url) - 1] = '\0';
    size_t len = strlen(server_url);
    if (len > 0 && server_url[len - 1] == '/') {
        server_url[len - 1] = '\0';
    }

    // Build URL: GET /api/v1/update/check?current=VERSION&platform=nds
    char url[512];
    snprintf(url, sizeof(url), "%s/api/v1/update/check?current=%s&platform=nds",
        server_url, APP_VERSION);

    iprintf("Checking for updates...\n");

    HttpResponse resp = http_request(url, HTTP_GET, state->api_key, NULL, 0);

    if (!resp.success || resp.status_code != 200) {
        iprintf("HTTP %d\n", resp.status_code);
        http_response_free(&resp);
        return false;
    }

    if (!resp.body || resp.body_size == 0) {
        iprintf("Empty response\n");
        http_response_free(&resp);
        return false;
    }

    // Null-terminate for JSON parsing
    char *json = (char *)malloc(resp.body_size + 1);
    if (!json) {
        http_response_free(&resp);
        return false;
    }
    memcpy(json, resp.body, resp.body_size);
    json[resp.body_size] = '\0';
    http_response_free(&resp);

    // Parse server response
    info->available = json_get_bool(json, "available");
    json_get_string(json, "latest_version", info->latest_version, sizeof(info->latest_version));
    json_get_string(json, "download_url", info->download_url, sizeof(info->download_url));
    json_get_string(json, "changelog", info->release_notes, sizeof(info->release_notes));
    info->file_size = json_get_int(json, "file_size");

    free(json);
    return true;
}

bool update_download(SyncState *state, const char *url, void (*progress_cb)(int percent)) {
    // Strip trailing slash from server_url
    char server_url[256];
    strncpy(server_url, state->server_url, sizeof(server_url) - 1);
    server_url[sizeof(server_url) - 1] = '\0';
    size_t len = strlen(server_url);
    if (len > 0 && server_url[len - 1] == '/') {
        server_url[len - 1] = '\0';
    }

    // Build proxy download URL
    char proxy_url[768];
    snprintf(proxy_url, sizeof(proxy_url), "%s/api/v1/update/download?url=%s",
        server_url, url);

    iprintf("Downloading update...\n");

    HttpResponse resp = http_request(proxy_url, HTTP_GET, state->api_key, NULL, 0);

    if (!resp.success || resp.status_code != 200) {
        iprintf("HTTP %d\n", resp.status_code);
        http_response_free(&resp);
        return false;
    }

    if (!resp.body || resp.body_size == 0) {
        iprintf("Empty download\n");
        http_response_free(&resp);
        return false;
    }

    // Create directory if needed
    mkdir("sd:/dssync", 0777);

    // Write to temporary file
    FILE *f = fopen(UPDATE_NDS_PATH, "wb");
    if (!f) {
        iprintf("Failed to create file\n");
        http_response_free(&resp);
        return false;
    }

    size_t written = fwrite(resp.body, 1, resp.body_size, f);
    fclose(f);

    if (written != resp.body_size) {
        iprintf("Write incomplete\n");
        remove(UPDATE_NDS_PATH);
        http_response_free(&resp);
        return false;
    }

    if (progress_cb) {
        progress_cb(100);
    }

    iprintf("Downloaded %zu bytes\n", written);
    http_response_free(&resp);
    return true;
}

bool update_apply_pending(void) {
    // Check if update file exists
    FILE *f = fopen(UPDATE_NDS_PATH, "rb");
    if (!f) {
        return false;  // No pending update
    }
    fclose(f);

    iprintf("Pending update found!\n");
    iprintf("Applying update...\n\n");

    // Try to determine the current running path
    // Common locations for DS homebrew
    const char *possible_paths[] = {
        "sd:/ndssync.nds",
        "fat:/ndssync.nds",
        "sd:/apps/ndssync/ndssync.nds",
        "fat:/apps/ndssync/ndssync.nds",
        NULL
    };

    // Try to find existing file to replace
    const char *target_path = NULL;
    for (int i = 0; possible_paths[i]; i++) {
        FILE *test = fopen(possible_paths[i], "rb");
        if (test) {
            fclose(test);
            target_path = possible_paths[i];
            break;
        }
    }

    if (!target_path) {
        // Default to root if we can't find it
        target_path = "sd:/ndssync.nds";
        iprintf("Using default path:\n%s\n\n", target_path);
    } else {
        iprintf("Found existing:\n%s\n\n", target_path);
    }

    // Create backup
    char backup_path[128];
    snprintf(backup_path, sizeof(backup_path), "%s.bak", target_path);
    remove(backup_path);  // Remove old backup
    rename(target_path, backup_path);

    // Copy update file to target location
    FILE *src = fopen(UPDATE_NDS_PATH, "rb");
    if (!src) {
        iprintf("Failed to open update\n");
        // Restore backup
        rename(backup_path, target_path);
        return false;
    }

    FILE *dst = fopen(target_path, "wb");
    if (!dst) {
        iprintf("Failed to create target\n");
        fclose(src);
        // Restore backup
        rename(backup_path, target_path);
        return false;
    }

    // Copy file
    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes, dst) != bytes) {
            iprintf("Write failed\n");
            fclose(src);
            fclose(dst);
            // Restore backup
            remove(target_path);
            rename(backup_path, target_path);
            return false;
        }
    }

    fclose(src);
    fclose(dst);

    // Remove update file
    remove(UPDATE_NDS_PATH);

    iprintf("Update applied!\n");
    iprintf("Backup saved to:\n%s\n\n", backup_path);
    iprintf("Please restart\n");

    return true;
}
