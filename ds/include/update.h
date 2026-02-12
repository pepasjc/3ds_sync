#ifndef UPDATE_H
#define UPDATE_H

#include "common.h"
#include <stdbool.h>

// APP_VERSION is defined by the Makefile from the root VERSION file
#define UPDATE_NDS_PATH "sd:/dssync/ndssync_update.nds"

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
bool update_apply_pending(void);

#endif
