#ifndef UPDATE_H
#define UPDATE_H

#include "common.h"
#include <stdbool.h>

// APP_VERSION is defined by the Makefile from the root VERSION file
// No device prefix — libfat routes to the default device
// (fat:/ on flashcard, sd:/ on DSi)
#define UPDATE_NDS_PATH "/dssync/ndssync_update.nds"

typedef struct {
    bool available;
    char latest_version[32];
    char download_url[256];
    char release_notes[512];
    size_t file_size;
} UpdateInfo;

// Check for updates via server proxy
bool update_check(SyncState *state, UpdateInfo *info);

// Download update file via server proxy
bool update_download(SyncState *state, const char *url, void (*progress_cb)(int percent));

// Apply pending update (called on startup)
// self_path: argv[0] from homebrew loader (path to running executable), or NULL
bool update_apply_pending(const char *self_path);

#endif
