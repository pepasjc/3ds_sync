#ifndef SYNC_H
#define SYNC_H

#include "common.h"
#include "title.h"

// Sync result for UI feedback
typedef enum {
    SYNC_OK,
    SYNC_ERR_NETWORK,
    SYNC_ERR_SERVER,
    SYNC_ERR_ARCHIVE,
    SYNC_ERR_BUNDLE,
} SyncResult;

// Callback for progress updates during sync
typedef void (*SyncProgressCb)(const char *message);

// Sync a single title with the server.
SyncResult sync_title(const AppConfig *config, const TitleInfo *title,
                      SyncProgressCb progress);

// Sync all titles: sends metadata to /sync endpoint, then
// uploads/downloads as directed by the sync plan.
// Returns number of titles synced, or -1 on error.
int sync_all(const AppConfig *config, const TitleInfo *titles, int title_count,
             SyncProgressCb progress);

#endif // SYNC_H
