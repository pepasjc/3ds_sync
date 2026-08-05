#ifndef WIIUSYNC_ROMS_H
#define WIIUSYNC_ROMS_H

#include "common.h"

/*
 * ROM catalog client.  Two systems are downloadable on a Wii U:
 *
 *   GC   — Nintendont layout:  sd:/games/<GAMEID6>/game.iso
 *          The catalog does not expose the disc's game code, so the download
 *          lands on a temporary name and is moved into place once the ISO
 *          header is readable (see roms_gameid_from_iso).
 *
 *   WII  — USB-loader layout:  sd:/wbfs/<Name> [ID6]/<ID6>.wbfs (+ .wbf1)
 *          The server converts RVZ -> ISO -> split WBFS and advertises the
 *          parts through /roms/{id}/wbfs-manifest; each part becomes its own
 *          resumable download entry.
 */

#define ROM_CATALOG_MAX 2048
#define ROM_ID_LEN      96

typedef struct {
    char     rom_id[ROM_ID_LEN];
    char     filename[160];
    char     name[MAX_TITLE_LEN];
    char     system[8];          /* "GC" / "WII" */
    uint64_t size;
    /* Server extract hint. RVZ entries advertise a format we pass back as
     * ?extract=<fmt> to get an ISO. Empty = raw download. */
    char     extract_format[8];
} RomEntry;

typedef struct {
    RomEntry items[ROM_CATALOG_MAX];
    int      count;
    char     system[8];          /* the system this catalog was fetched for */
    char     last_error[160];
} RomCatalog;

bool roms_fetch_catalog(const SyncState *state,
                        const char *system_code,
                        char *scratch_buf, uint32_t scratch_buf_size,
                        RomCatalog *catalog);

/* ?extract= value for this entry (whatever the server advertised). */
const char *roms_preferred_extract_format(const RomEntry *rom);

/* Target/storage config (set at boot and whenever the folders change). */
void roms_set_target(const SyncState *state);
const char *roms_downloads_file(void);
const char *roms_games_dir(char *out, size_t out_size);   /* "<sd>/games" */
const char *roms_wbfs_dir(char *out, size_t out_size);    /* "<sd>/wbfs"  */
void roms_ensure_target_dirs(void);
void roms_mkdir_p(const char *path);

/* Staging path for a GC download: <games>/_dl/<sanitised rom_id>.iso.
 * roms_install_gc_iso() moves it to <games>/<GAMEID6>/game.iso afterwards. */
bool roms_resolve_gc_staging_path(const RomEntry *rom, char *out, size_t out_size);

/* Read the 6-char game id from a GameCube ISO header (bytes 0-5), verifying
 * the 0xC2339F3D magic at 0x1C.  Returns true on success. */
bool roms_gameid_from_iso(const char *iso_path, char *out_id6, size_t out_size);

/* Move a completed staging ISO into Nintendont's <games>/<GAMEID6>/game.iso.
 * ``msg`` receives a result line. Returns 0 on success. */
int  roms_install_gc_iso(const char *staging_path, char *msg, size_t msg_size);

/* Directory a Wii game's split WBFS parts belong in:
 * <wbfs>/<Name> [<ID6>].  Created on demand. */
void roms_wbfs_game_dir(const char *name, const char *id6,
                        char *out, size_t out_size);

/* ---- installed games on SD ---- */

#define LOCAL_ROMS_MAX 1024

typedef struct {
    char     name[MAX_TITLE_LEN];
    char     filename[200];
    char     path[SAVE_DIR_LEN];
    char     system[8];
    uint64_t size;
} LocalRom;

typedef struct {
    LocalRom items[LOCAL_ROMS_MAX];
    int      count;
    char     last_error[160];
} LocalRomList;

/* Walk <games>/<GAMEID6>/game.iso (Nintendont) and the .wbfs files under
 * <wbfs>/<dir>/. */
void roms_scan_local(LocalRomList *out);

#endif /* WIIUSYNC_ROMS_H */
