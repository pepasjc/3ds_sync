#ifndef PS2SYNC_ROMS_H
#define PS2SYNC_ROMS_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * PS2 catalog cap.  Real-world Redump set is ~5000 titles; allow 4096
 * which is comfortable in PS2's 32 MB RAM (RomEntry ≈ 480 B → ~2 MB BSS).
 */
#define ROM_CATALOG_MAX 4096

/* Server rom_id, e.g. "PS2_BUNDLE_xxx" or just "SLUS_213.71". */
#define ROM_ID_LEN 96

#define ROM_BUNDLE_FILE_MAX 8

typedef struct {
    char     name[160];
    uint64_t size;
} RomBundleFile;

typedef struct {
    char     rom_id[ROM_ID_LEN];
    char     filename[160];
    char     name[MAX_TITLE_LEN];
    char     system[8];          /* "PS2" */
    /* Disc serial (e.g. "SLUS_213.71").  Used as the on-disk filename
     * prefix so OPL can look up game art / per-game settings. */
    char     serial[GAME_ID_LEN];
    uint64_t size;
    bool     is_bundle;
    int      file_count;
    /* Server extract hint.  CHD entries advertise extract_format="ps2"
     * and we always pick "iso" — OPL doesn't read CHD, and CSO support
     * for PS2 is patchy.  Empty = raw download. */
    char     extract_format[8];
    /* CD vs DVD format hint (server-derived).  Picks mass:/CD/ vs
     * mass:/DVD/ destination. */
    bool     is_cd;
} RomEntry;

typedef struct {
    RomEntry items[ROM_CATALOG_MAX];
    int      count;
    char     last_error[128];
} RomCatalog;

bool roms_fetch_catalog(const SyncState *state,
                        const char *system_code,
                        char *scratch_buf, uint32_t scratch_buf_size,
                        RomCatalog *catalog);

/* Pick ?extract= value for this entry.  Defaults to "iso" for CHD
 * sources, "" for native ISO. */
const char *roms_preferred_extract_format(const RomEntry *rom);

/*
 * On-disk path for an entry.  Format follows OPL convention:
 *   mass:/DVD/<SERIAL>.<title>.iso
 *   mass:/CD/<SERIAL>.<title>.iso
 *
 * Title sanitised to ASCII a-z A-Z 0-9 space.  Truncated so the full
 * path fits in 256 bytes.  Returns false if out_size too small.
 */
bool roms_resolve_target_path(const RomEntry *rom,
                              char *out_path, size_t out_size);

void roms_set_usb_root(const char *root);
const char *roms_usb_root(void);
const char *roms_downloads_file(void);
void roms_usb_data_dir(char *out_path, size_t out_size);

void roms_ensure_target_dirs(void);
void roms_mkdir_p(const char *path);

/*
 * Local ISO catalog — what's already on the USB stick.
 *
 * Scans mass:/DVD and mass:/CD for files matching the OPL naming
 * convention `<SERIAL>.<title>.iso`.  Used by the Local view so the
 * user can see at a glance what's already installed without having
 * to fetch the server catalog.
 */
#define LOCAL_ROMS_MAX 1024

typedef struct {
    char serial[GAME_ID_LEN];     /* "SLUS_200.74" */
    char name[MAX_TITLE_LEN];     /* "Summoner" (parsed from filename) */
    char filename[200];           /* full leaf filename */
    char path[260];               /* full path: mass:/DVD/... */
    bool is_cd;
    uint64_t size;
} LocalRom;

typedef struct {
    LocalRom items[LOCAL_ROMS_MAX];
    int      count;
    char     last_error[128];
} LocalRomList;

void roms_scan_local(LocalRomList *out);

#endif /* PS2SYNC_ROMS_H */
