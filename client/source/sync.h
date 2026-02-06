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
    SYNC_ERR_TOO_LARGE,
} SyncResult;

// Summary of sync_all operation
#define MAX_CONFLICT_DISPLAY 8  // Max conflicts to report for UI

typedef struct {
    int uploaded;
    int downloaded;
    int up_to_date;
    int conflicts;
    int failed;
    int skipped;       // server_only titles not on this device
    // First few conflicting title IDs for display (null-terminated strings)
    char conflict_titles[MAX_CONFLICT_DISPLAY][17];
} SyncSummary;

// Callback for progress updates during sync
typedef void (*SyncProgressCb)(const char *message);

// Get human-readable error message for a SyncResult
const char *sync_result_str(SyncResult result);

// Sync a single title with the server.
SyncResult sync_title(const AppConfig *config, const TitleInfo *title,
                      SyncProgressCb progress);

// Sync all titles: sends metadata to /sync endpoint, then
// uploads/downloads as directed by the sync plan.
// Returns true on success (even if some titles failed), false on fatal error.
// Fills summary with counts if non-NULL.
bool sync_all(const AppConfig *config, const TitleInfo *titles, int title_count,
              SyncProgressCb progress, SyncSummary *summary);

// Download a specific title from the server (force download, ignore local state)
SyncResult sync_download_title(const AppConfig *config, const TitleInfo *title,
                               SyncProgressCb progress);

// Save details info (for details dialog)
typedef struct {
    // Local info
    int local_file_count;
    u32 local_size;
    char local_hash[65];
    bool local_exists;

    // Server info (fetched from server)
    bool server_exists;
    int server_file_count;
    u32 server_size;
    char server_hash[65];
    char server_last_sync[32];      // ISO 8601 date
    char server_console_id[17];     // Which console uploaded

    // Sync status
    bool is_synced;           // local_hash == server_hash
    bool has_last_synced;     // Whether we have a sync state file
    char last_synced_hash[65];
} SaveDetails;

// Get detailed info about a save (local + server).
// Fills details struct with information. Returns true on success.
bool sync_get_save_details(const AppConfig *config, const TitleInfo *title,
                           SaveDetails *details);

#endif // SYNC_H
