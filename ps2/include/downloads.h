#ifndef PS2SYNC_DOWNLOADS_H
#define PS2SYNC_DOWNLOADS_H

#include "common.h"
#include "roms.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Pause/resume download manager.  Same shape as the PSP / PS3 clients —
 * flat key=value file on the detected USB root, in-RAM mirror, atomic
 * save via tmp+rename.
 */

#define DOWNLOAD_MAX 64
#define DOWNLOAD_NAME_LEN 96
#define DOWNLOAD_PATH_LEN 320
#define DOWNLOAD_STATUS_LEN 16

typedef enum {
    DL_STATUS_QUEUED    = 0,
    DL_STATUS_ACTIVE    = 1,
    DL_STATUS_PAUSED    = 2,
    DL_STATUS_COMPLETED = 3,
    DL_STATUS_ERROR     = 4,
} DownloadStatus;

typedef struct {
    char     rom_id[ROM_ID_LEN];
    char     filename[160];
    char     name[DOWNLOAD_NAME_LEN];
    char     target_path[DOWNLOAD_PATH_LEN];
    char     system[8];
    char     serial[GAME_ID_LEN];
    uint64_t offset;
    uint64_t total;
    DownloadStatus status;
    char     extract_format[8];
    bool     is_cd;
} DownloadEntry;

typedef struct {
    DownloadEntry items[DOWNLOAD_MAX];
    int           count;
} DownloadList;

const char *downloads_status_to_str(DownloadStatus s);
DownloadStatus downloads_str_to_status(const char *s);

bool downloads_load(DownloadList *list);
bool downloads_save(const DownloadList *list);

DownloadEntry *downloads_find(DownloadList *list, const char *rom_id);
DownloadEntry *downloads_upsert_from_catalog(DownloadList *list,
                                             const RomEntry *rom);
bool downloads_remove(DownloadList *list, const char *rom_id);
DownloadEntry *downloads_next_runnable(DownloadList *list);

#endif /* PS2SYNC_DOWNLOADS_H */
