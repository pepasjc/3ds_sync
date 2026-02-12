#ifndef NETWORK_H
#define NETWORK_H

#include "common.h"

// Initialize WiFi (for DS/DSi with WiFi)
int network_init(SyncState *state);

// Sync with server and get plan
int network_sync(SyncState *state);

// Fetch save list from server (stores in state->server_saves)
int network_fetch_saves(SyncState *state);

// Get save info from server (hash, size) - returns 0 on success, -1 if not found
// title_id_hex should be 16-char uppercase hex string like "0004800041324445"
int network_get_save_info(SyncState *state, const char *title_id_hex, char *hash_out, size_t *size_out);

// Extended version that also returns client_timestamp from server metadata
int network_get_save_info_ext(SyncState *state, const char *title_id_hex,
                               char *hash_out, size_t *size_out, uint32_t *timestamp_out);

// Upload save bundle
int network_upload(SyncState *state, int title_idx);

// Download save bundle
int network_download(SyncState *state, int title_idx);

// Cleanup network
void network_cleanup(void);

#endif
