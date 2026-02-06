#include "sync.h"
#include "archive.h"
#include "bundle.h"
#include "network.h"
#include "sha256.h"

#include <inttypes.h>
#include <sys/stat.h>

#define MAX_SAVE_FILES 64
#define STATE_DIR "sdmc:/3ds/3dssync/state"

// Load the last synced hash for a title from the state file.
// Returns true and fills hash_out (65 bytes) on success.
static bool load_last_synced_hash(const char *title_id_hex, char *hash_out) {
    char path[256];
    snprintf(path, sizeof(path), STATE_DIR "/%s.txt", title_id_hex);

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char buf[65] = {0};
    size_t rd = fread(buf, 1, 64, f);
    fclose(f);

    if (rd != 64) return false;

    // Validate hex characters
    for (int i = 0; i < 64; i++) {
        char c = buf[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }

    memcpy(hash_out, buf, 64);
    hash_out[64] = '\0';
    return true;
}

// Save the current hash as the last synced hash for a title.
static bool save_last_synced_hash(const char *title_id_hex, const char *hash) {
    // Ensure directories exist
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/3dssync", 0777);
    mkdir(STATE_DIR, 0777);

    char path[256];
    snprintf(path, sizeof(path), STATE_DIR "/%s.txt", title_id_hex);

    FILE *f = fopen(path, "w");
    if (!f) return false;

    size_t written = fwrite(hash, 1, 64, f);
    fclose(f);

    return written == 64;
}

// Minimal JSON string search - find value for a key in a JSON string.
// Returns pointer to the start of the value (after the colon and quote).
// Only handles simple string/number values, not nested objects.
static const char *json_find_key(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return NULL;
    pos += strlen(search);
    // Skip : and whitespace
    while (*pos == ':' || *pos == ' ' || *pos == '\t') pos++;
    return pos;
}

// Parse a JSON array of strings, returns count. Fills out_items.
static int json_parse_string_array(const char *json, const char *key,
                                   char out_items[][17], int max_items) {
    const char *arr = json_find_key(json, key);
    if (!arr || *arr != '[') return 0;
    arr++; // skip '['

    int count = 0;
    while (*arr && *arr != ']' && count < max_items) {
        // Find next quoted string
        const char *q1 = strchr(arr, '"');
        if (!q1) break;
        q1++;
        const char *q2 = strchr(q1, '"');
        if (!q2) break;

        int len = (int)(q2 - q1);
        if (len > 0 && len <= 16) {
            memcpy(out_items[count], q1, len);
            out_items[count][len] = '\0';
            count++;
        }
        arr = q2 + 1;
    }
    return count;
}

// Build JSON metadata for one title
static int build_title_json(char *buf, int buf_size, const TitleInfo *title,
                            const char *hash, u32 total_size,
                            const char *last_synced_hash) {
    // Get a timestamp (seconds since 2000-01-01 from 3DS, convert to rough unix)
    u64 ms = osGetTime(); // ms since Jan 1 2000
    u32 timestamp = (u32)(ms / 1000) + 946684800; // add seconds from 1970 to 2000

    if (last_synced_hash && last_synced_hash[0] != '\0') {
        return snprintf(buf, buf_size,
            "{\"title_id\":\"%s\",\"save_hash\":\"%s\","
            "\"timestamp\":%lu,\"size\":%lu,\"last_synced_hash\":\"%s\"}",
            title->title_id_hex, hash,
            (unsigned long)timestamp, (unsigned long)total_size,
            last_synced_hash);
    } else {
        return snprintf(buf, buf_size,
            "{\"title_id\":\"%s\",\"save_hash\":\"%s\","
            "\"timestamp\":%lu,\"size\":%lu}",
            title->title_id_hex, hash,
            (unsigned long)timestamp, (unsigned long)total_size);
    }
}

static SyncResult upload_title_with_hash(const AppConfig *config, const TitleInfo *title,
                                         SyncProgressCb progress, const char *save_hash) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Reading save: %s", title->title_id_hex);
    if (progress) progress(msg);

    // Heap-allocate to avoid stack overflow
    ArchiveFile *files = (ArchiveFile *)malloc(MAX_SAVE_FILES * sizeof(ArchiveFile));
    if (!files) return SYNC_ERR_ARCHIVE;

    int file_count = archive_read(title->title_id, title->media_type,
                                  files, MAX_SAVE_FILES);
    if (file_count < 0) { free(files); return SYNC_ERR_ARCHIVE; }
    if (file_count == 0) { free(files); return SYNC_OK; }

    // Compute hash if not provided
    char computed_hash[65] = {0};
    const char *hash_to_save = save_hash;
    if (!hash_to_save || hash_to_save[0] == '\0') {
        bundle_compute_save_hash(files, file_count, computed_hash);
        hash_to_save = computed_hash;
    }

    snprintf(msg, sizeof(msg), "Uploading: %s (%d files)", title->title_id_hex, file_count);
    if (progress) progress(msg);

    // Create bundle
    u64 ms = osGetTime();
    u32 timestamp = (u32)(ms / 1000) + 946684800;
    u32 bundle_size;
    u8 *bundle = bundle_create(title->title_id, timestamp,
                               files, file_count, &bundle_size);
    archive_free_files(files, file_count);
    free(files);

    if (!bundle) return SYNC_ERR_BUNDLE;

    // POST to server
    char path[64];
    snprintf(path, sizeof(path), "/saves/%s", title->title_id_hex);

    u32 resp_size, status;
    u8 *resp = network_post(config, path, bundle, bundle_size, &resp_size, &status);
    free(bundle);

    if (!resp) return SYNC_ERR_NETWORK;
    free(resp);

    if (status == 200) {
        // Upload succeeded - save this hash as last synced state
        save_last_synced_hash(title->title_id_hex, hash_to_save);
        return SYNC_OK;
    }
    return SYNC_ERR_SERVER;
}

static SyncResult download_title(const AppConfig *config, const TitleInfo *title,
                                 SyncProgressCb progress) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Downloading: %s", title->title_id_hex);
    if (progress) progress(msg);

    char path[64];
    snprintf(path, sizeof(path), "/saves/%s", title->title_id_hex);

    u32 resp_size, status;
    u8 *resp = network_get(config, path, &resp_size, &status);
    if (!resp) return SYNC_ERR_NETWORK;
    if (status != 200) { free(resp); return SYNC_ERR_SERVER; }

    // Heap-allocate to avoid stack overflow
    ArchiveFile *files = (ArchiveFile *)malloc(MAX_SAVE_FILES * sizeof(ArchiveFile));
    if (!files) { free(resp); return SYNC_ERR_BUNDLE; }

    u64 tid;
    u32 ts;
    int file_count = bundle_parse(resp, resp_size, &tid, &ts, files, MAX_SAVE_FILES);
    if (file_count < 0) { free(files); free(resp); return SYNC_ERR_BUNDLE; }

    // Compute hash of downloaded save (before write, while data is valid)
    char new_hash[65];
    bundle_compute_save_hash(files, file_count, new_hash);

    snprintf(msg, sizeof(msg), "Writing save: %s (%d files)", title->title_id_hex, file_count);
    if (progress) progress(msg);

    // Write to save archive
    bool ok = archive_write(title->title_id, title->media_type, files, file_count);
    free(files);
    free(resp); // frees the parsed file data too (pointers into resp)

    if (ok) {
        // Download and write succeeded - save this hash as last synced state
        save_last_synced_hash(title->title_id_hex, new_hash);
        return SYNC_OK;
    }
    return SYNC_ERR_ARCHIVE;
}

SyncResult sync_title(const AppConfig *config, const TitleInfo *title,
                      SyncProgressCb progress) {
    // For single-title sync: always upload (the server will reject if older)
    // Pass NULL for hash - upload_title_with_hash will compute it
    return upload_title_with_hash(config, title, progress, NULL);
}

int sync_all(const AppConfig *config, const TitleInfo *titles, int title_count,
             SyncProgressCb progress) {
    if (progress) progress("Preparing sync metadata...");

    // Cache for computed hashes (needed for upload later)
    char (*hash_cache)[65] = (char (*)[65])malloc(title_count * 65);
    if (!hash_cache) return -1;

    // Heap-allocate files array to avoid stack overflow
    ArchiveFile *files = (ArchiveFile *)malloc(MAX_SAVE_FILES * sizeof(ArchiveFile));
    if (!files) { free(hash_cache); return -1; }

    // Build JSON for sync request
    // Estimate: ~230 bytes per title (with last_synced_hash) + overhead
    int json_cap = title_count * 230 + 64;
    char *json = (char *)malloc(json_cap);
    if (!json) { free(files); free(hash_cache); return -1; }

    int pos = snprintf(json, json_cap, "{\"titles\":[");

    for (int i = 0; i < title_count; i++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Hashing save %d/%d: %s",
            i + 1, title_count, titles[i].title_id_hex);
        if (progress) progress(msg);

        // Read save to compute current hash
        int fc = archive_read(titles[i].title_id, titles[i].media_type,
                              files, MAX_SAVE_FILES);
        if (fc < 0) fc = 0;

        char current_hash[65] = {0};
        u32 total_size = 0;
        if (fc > 0) {
            bundle_compute_save_hash(files, fc, current_hash);
            for (int j = 0; j < fc; j++) total_size += files[j].size;
            archive_free_files(files, fc);
        } else {
            strcpy(current_hash, "0000000000000000000000000000000000000000000000000000000000000000");
        }

        // Cache this hash for potential upload later
        strcpy(hash_cache[i], current_hash);

        // Load last synced hash (if exists)
        char last_synced[65] = {0};
        bool has_last_synced = load_last_synced_hash(titles[i].title_id_hex, last_synced);

        if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
        pos += build_title_json(json + pos, json_cap - pos, &titles[i],
                               current_hash, total_size,
                               has_last_synced ? last_synced : NULL);
    }

    pos += snprintf(json + pos, json_cap - pos, "]}");

    // Done with files array for hashing phase
    free(files);

    // Send sync request
    if (progress) progress("Sending sync request...");

    u32 resp_size, status;
    u8 *resp = network_post_json(config, "/sync", json, &resp_size, &status);
    free(json);

    if (!resp) { free(hash_cache); return -1; }
    if (status != 200) { free(resp); free(hash_cache); return -1; }

    // Null-terminate response for string parsing
    u8 *resp_str = (u8 *)realloc(resp, resp_size + 1);
    if (!resp_str) { free(resp); free(hash_cache); return -1; }
    resp_str[resp_size] = '\0';
    char *plan = (char *)resp_str;

    // Parse sync plan - heap allocate to avoid stack overflow (256 * 17 * 3 = 13KB)
    char (*upload_ids)[17] = (char (*)[17])malloc(MAX_TITLES * 17);
    char (*download_ids)[17] = (char (*)[17])malloc(MAX_TITLES * 17);
    char (*server_only_ids)[17] = (char (*)[17])malloc(MAX_TITLES * 17);

    if (!upload_ids || !download_ids || !server_only_ids) {
        free(upload_ids);
        free(download_ids);
        free(server_only_ids);
        free(resp_str);
        free(hash_cache);
        return -1;
    }

    int upload_count = json_parse_string_array(plan, "upload", upload_ids, MAX_TITLES);
    int download_count = json_parse_string_array(plan, "download", download_ids, MAX_TITLES);
    int server_only_count = json_parse_string_array(plan, "server_only", server_only_ids, MAX_TITLES);

    free(resp_str);

    int synced = 0;
    char msg[128];

    // Process uploads
    for (int i = 0; i < upload_count; i++) {
        // Find the title and its cached hash
        for (int j = 0; j < title_count; j++) {
            if (strcmp(titles[j].title_id_hex, upload_ids[i]) == 0) {
                snprintf(msg, sizeof(msg), "Uploading %d/%d: %s",
                    i + 1, upload_count, upload_ids[i]);
                if (progress) progress(msg);

                if (upload_title_with_hash(config, &titles[j], NULL, hash_cache[j]) == SYNC_OK)
                    synced++;
                break;
            }
        }
    }

    // Process downloads (both "download" and "server_only")
    int total_dl = download_count + server_only_count;
    int dl_done = 0;

    for (int i = 0; i < download_count; i++) {
        for (int j = 0; j < title_count; j++) {
            if (strcmp(titles[j].title_id_hex, download_ids[i]) == 0) {
                snprintf(msg, sizeof(msg), "Downloading %d/%d: %s",
                    ++dl_done, total_dl, download_ids[i]);
                if (progress) progress(msg);

                if (download_title(config, &titles[j], NULL) == SYNC_OK)
                    synced++;
                break;
            }
        }
    }

    // server_only titles also need download, but only if title exists locally
    for (int i = 0; i < server_only_count; i++) {
        for (int j = 0; j < title_count; j++) {
            if (strcmp(titles[j].title_id_hex, server_only_ids[i]) == 0) {
                snprintf(msg, sizeof(msg), "Downloading %d/%d: %s",
                    ++dl_done, total_dl, server_only_ids[i]);
                if (progress) progress(msg);

                if (download_title(config, &titles[j], NULL) == SYNC_OK)
                    synced++;
                break;
            }
        }
    }

    free(upload_ids);
    free(download_ids);
    free(server_only_ids);
    free(hash_cache);
    return synced;
}
