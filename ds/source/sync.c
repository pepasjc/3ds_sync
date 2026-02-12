#include "sync.h"
#include "saves.h"
#include "network.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// State file path prefixes (same as config)
static const char *state_prefixes[] = {
    "sd:/dssync/state",
    "fat:/dssync/state",
    "/dssync/state",
    "sdmc:/dssync/state",
    NULL
};

// Cached state directory (found on first use)
static char state_dir[256] = "";
static bool state_dir_found = false;

// Convert 32-byte binary hash to 64-char hex string
static void hash_to_hex(const uint8_t hash[32], char hex[65]) {
    for (int i = 0; i < 32; i++) {
        sprintf(&hex[i * 2], "%02x", hash[i]);
    }
    hex[64] = '\0';
}

// Format title_id bytes as 16-char uppercase hex string
static void title_id_to_hex(const uint8_t title_id[8], char hex[17]) {
    snprintf(hex, 17, "%02X%02X%02X%02X%02X%02X%02X%02X",
        title_id[0], title_id[1], title_id[2], title_id[3],
        title_id[4], title_id[5], title_id[6], title_id[7]);
}

// Find or create the state directory
static bool ensure_state_dir(void) {
    if (state_dir_found) return true;

    struct stat st;
    for (int i = 0; state_prefixes[i]; i++) {
        // Check if directory already exists
        if (stat(state_prefixes[i], &st) == 0) {
            strncpy(state_dir, state_prefixes[i], sizeof(state_dir) - 1);
            state_dir_found = true;
            return true;
        }

        // Try to create parent "dssync" then "dssync/state"
        char parent[256];
        // Extract prefix (everything before /dssync/state)
        strncpy(parent, state_prefixes[i], sizeof(parent) - 1);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';  // Now "sd:/dssync" etc.
            mkdir(parent, 0777);
        }

        if (mkdir(state_prefixes[i], 0777) == 0 || stat(state_prefixes[i], &st) == 0) {
            strncpy(state_dir, state_prefixes[i], sizeof(state_dir) - 1);
            state_dir_found = true;
            return true;
        }
    }

    return false;
}

bool sync_load_last_hash(const char *title_id_hex, char *hash_out) {
    if (!ensure_state_dir()) return false;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.txt", state_dir, title_id_hex);

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char buf[65] = {0};
    size_t rd = fread(buf, 1, 64, f);
    fclose(f);

    if (rd != 64) return false;

    // Validate all hex characters
    for (int i = 0; i < 64; i++) {
        char c = buf[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }

    memcpy(hash_out, buf, 64);
    hash_out[64] = '\0';
    return true;
}

bool sync_save_last_hash(const char *title_id_hex, const char *hash) {
    if (!ensure_state_dir()) return false;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.txt", state_dir, title_id_hex);

    FILE *f = fopen(path, "w");
    if (!f) return false;

    size_t written = fwrite(hash, 1, 64, f);
    fclose(f);

    return written == 64;
}

int sync_decide(SyncState *state, int title_idx, SyncDecision *decision) {
    Title *title = &state->titles[title_idx];
    memset(decision, 0, sizeof(SyncDecision));

    bool has_local = (title->save_size > 0);

    // Step 1: Ensure local hash is computed
    if (has_local) {
        if (saves_ensure_hash(title) != 0) {
            return -1;
        }
    }

    // Step 2: Get title_id hex and load last synced hash
    char title_id_hex[17];
    title_id_to_hex(title->title_id, title_id_hex);

    decision->has_last_synced = sync_load_last_hash(title_id_hex, decision->last_synced_hash);

    // Step 3: Fetch server metadata
    int server_result = network_get_save_info_ext(state, title_id_hex,
        decision->server_hash, &decision->server_size, &decision->server_timestamp);
    bool has_server = (server_result == 0);

    // Step 4: Get local mtime
    decision->local_mtime = title->timestamp;

    // Step 5: Convert local hash to hex for comparison
    char local_hash_hex[65] = "";
    if (has_local && title->hash_calculated) {
        hash_to_hex(title->hash, local_hash_hex);
    }

    // Step 6: Three-way decision logic
    if (!has_local && !has_server) {
        decision->action = SYNC_UP_TO_DATE;
        return 0;
    }

    if (has_local && !has_server) {
        decision->action = SYNC_UPLOAD;
        return 0;
    }

    if (!has_local && has_server) {
        decision->action = SYNC_DOWNLOAD;
        return 0;
    }

    // Both exist — compare hashes
    if (strncasecmp(local_hash_hex, decision->server_hash, 64) == 0) {
        decision->action = SYNC_UP_TO_DATE;
        return 0;
    }

    // Hashes differ — use three-way comparison
    if (decision->has_last_synced) {
        if (strncasecmp(decision->last_synced_hash, decision->server_hash, 64) == 0) {
            // Server unchanged, only local changed
            decision->action = SYNC_UPLOAD;
        } else if (strncasecmp(decision->last_synced_hash, local_hash_hex, 64) == 0) {
            // Local unchanged, only server changed
            decision->action = SYNC_DOWNLOAD;
        } else {
            // All three differ
            decision->action = SYNC_CONFLICT;
        }
    } else {
        // No sync history — use mtime as fallback hint
        if (decision->local_mtime > 0 && decision->server_timestamp > 0) {
            if (decision->local_mtime > decision->server_timestamp) {
                decision->action = SYNC_UPLOAD;
            } else if (decision->local_mtime < decision->server_timestamp) {
                decision->action = SYNC_DOWNLOAD;
            } else {
                decision->action = SYNC_CONFLICT;
            }
        } else {
            // No reliable timestamps — conflict
            decision->action = SYNC_CONFLICT;
        }
    }

    return 0;
}

int sync_execute(SyncState *state, int title_idx, SyncAction action) {
    Title *title = &state->titles[title_idx];
    char title_id_hex[17];
    title_id_to_hex(title->title_id, title_id_hex);

    int result = -1;

    switch (action) {
        case SYNC_UPLOAD:
            result = network_upload(state, title_idx);
            break;

        case SYNC_DOWNLOAD:
            result = network_download(state, title_idx);
            break;

        case SYNC_UP_TO_DATE:
            result = 0;
            break;

        case SYNC_CONFLICT:
            return -1;  // Caller must resolve
    }

    if (result == 0) {
        // Save current hash as last synced
        // After download, network_download recalculates title->hash
        if (title->hash_calculated) {
            char hash_hex[65];
            hash_to_hex(title->hash, hash_hex);
            sync_save_last_hash(title_id_hex, hash_hex);
        } else if (saves_ensure_hash(title) == 0) {
            char hash_hex[65];
            hash_to_hex(title->hash, hash_hex);
            sync_save_last_hash(title_id_hex, hash_hex);
        }
    }

    return result;
}

int sync_all(SyncState *state, SyncSummary *summary) {
    memset(summary, 0, sizeof(SyncSummary));

    for (int i = 0; i < state->num_titles; i++) {
        Title *title = &state->titles[i];

        // Skip titles with no local save and not on server
        if (title->save_size == 0) {
            // Check if server has it by trying to decide
            SyncDecision decision;
            if (sync_decide(state, i, &decision) != 0) {
                summary->failed++;
                iprintf("  %s: FAILED\n", title->game_name);
                continue;
            }

            if (decision.action == SYNC_UP_TO_DATE) {
                summary->up_to_date++;
                continue;
            }

            if (decision.action == SYNC_DOWNLOAD) {
                iprintf("  %s: DL...", title->game_name);
                if (sync_execute(state, i, SYNC_DOWNLOAD) == 0) {
                    summary->downloaded++;
                    iprintf("OK\n");
                } else {
                    summary->failed++;
                    iprintf("FAIL\n");
                }
            }
            continue;
        }

        // Normal title with local save
        iprintf("  [%d/%d] %.20s\n", i + 1, state->num_titles, title->game_name);

        SyncDecision decision;
        if (sync_decide(state, i, &decision) != 0) {
            summary->failed++;
            iprintf("    -> FAILED\n");
            continue;
        }

        switch (decision.action) {
            case SYNC_UP_TO_DATE:
                summary->up_to_date++;
                iprintf("    -> Up to date\n");
                break;

            case SYNC_UPLOAD:
                iprintf("    -> Uploading...");
                if (sync_execute(state, i, SYNC_UPLOAD) == 0) {
                    summary->uploaded++;
                    iprintf("OK\n");
                } else {
                    summary->failed++;
                    iprintf("FAIL\n");
                }
                break;

            case SYNC_DOWNLOAD:
                iprintf("    -> Downloading...");
                if (sync_execute(state, i, SYNC_DOWNLOAD) == 0) {
                    summary->downloaded++;
                    iprintf("OK\n");
                } else {
                    summary->failed++;
                    iprintf("FAIL\n");
                }
                break;

            case SYNC_CONFLICT:
                summary->conflicts++;
                iprintf("    -> CONFLICT\n");
                break;
        }
    }

    return 0;
}
