#ifndef UPDATE_H
#define UPDATE_H

#include "common.h"

// Update check result
typedef struct {
    bool available;
    char latest_version[16];
    char download_url[256];
    u32 file_size;
} UpdateInfo;

// Check for updates from server.
// Returns true if check succeeded (regardless of whether update is available).
bool update_check(const AppConfig *config, UpdateInfo *info);

// Download update to SD card.
// Returns true on success.
// Progress callback receives percentage (0-100).
typedef void (*UpdateProgressCb)(int percent);
bool update_download(const AppConfig *config, const char *url, UpdateProgressCb progress);

// Install CIA from SD card using AM service.
// Returns true on success.
bool update_install(UpdateProgressCb progress);

#endif // UPDATE_H
