#ifndef PS2SYNC_HDL_H
#define PS2SYNC_HDL_H

#include "common.h"
#include "downloads.h"
#include "roms.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define HDL_PARTITION_NAME_LEN 33
#define HDL_MAX_PARTS 65

typedef struct {
    uint32_t start_lba;       /* absolute 512-byte sector */
    uint64_t image_offset;    /* byte offset in ISO stream */
    uint64_t capacity;        /* usable bytes in this APA partition */
} HdlRegion;

typedef struct {
    bool     active;
    char     partition[HDL_PARTITION_NAME_LEN];
    char     target_path[DOWNLOAD_PATH_LEN];
    char     game_name[MAX_TITLE_LEN];
    char     startup[GAME_ID_LEN];
    bool     is_cd;
    uint64_t image_size;
    uint64_t offset;
    int      region_count;
    HdlRegion regions[HDL_MAX_PARTS];
    uint8_t  pending[512];
    uint32_t pending_len;
} HdlInstall;

bool hdl_make_partition_name(const char *serial,
                             const char *title,
                             char *out, size_t out_size);
bool hdl_resolve_target_path_from_rom(const RomEntry *rom,
                                      char *out_path, size_t out_size);
bool hdl_resolve_target_path_from_download(const DownloadEntry *entry,
                                           char *out_path, size_t out_size);

void hdl_scan_local(LocalRomList *out);
int  hdl_remove_partition(const char *path);

int hdl_install_begin(HdlInstall *ctx,
                      const DownloadEntry *entry,
                      uint64_t content_length);
int hdl_install_write(const void *data, uint32_t len, void *user);
int hdl_install_finish(HdlInstall *ctx, bool success);

#endif /* PS2SYNC_HDL_H */
