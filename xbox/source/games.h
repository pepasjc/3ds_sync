#ifndef XBOX_GAMES_H
#define XBOX_GAMES_H

#include <stdint.h>

#include "config.h"

#define XBOX_ROM_ID_MAX     128
#define XBOX_ROM_NAME_MAX   96
#define XBOX_ROM_FILE_MAX   96
#define XBOX_MAX_ROMS       512
#define XBOX_INSTALLED_NAME_MAX 96
#define XBOX_INSTALLED_PATH_MAX 260
#define XBOX_MAX_INSTALLED_GAMES 256

typedef enum {
    XBOX_GAME_FORMAT_CCI = 0,
    XBOX_GAME_FORMAT_FOLDER = 1,
} XboxGameFormat;

typedef struct {
    char     rom_id[XBOX_ROM_ID_MAX];
    char     name[XBOX_ROM_NAME_MAX];
    char     filename[XBOX_ROM_FILE_MAX];
    uint64_t size;
    int      is_bundle;
} XboxRomEntry;

typedef struct {
    int count;
    XboxRomEntry roms[XBOX_MAX_ROMS];
} XboxRomList;

typedef struct {
    char     name[XBOX_INSTALLED_NAME_MAX];
    char     path[XBOX_INSTALLED_PATH_MAX];
    uint64_t size;
    uint32_t file_count;
    uint32_t dir_count;
} XboxInstalledGame;

typedef struct {
    int count;
    uint64_t total_size;
    XboxInstalledGame games[XBOX_MAX_INSTALLED_GAMES];
} XboxInstalledGameList;

typedef struct {
    uint64_t free_bytes;
    uint64_t total_bytes;
} XboxDriveSpace;

typedef void (*GameProgressFn)(const char *msg,
                               uint64_t done,
                               uint64_t total,
                               void *user);

XboxGameFormat games_config_format(const XboxConfig *cfg);
const char *games_format_name(XboxGameFormat fmt);

int games_mount_target(const XboxConfig *cfg, char *err, int err_len);
int games_fetch_catalog(const XboxConfig *cfg, XboxRomList *out,
                        char *err, int err_len);
int games_download_rom(const XboxConfig *cfg,
                       const XboxRomEntry *rom,
                       XboxGameFormat fmt,
                       GameProgressFn progress,
                       void *progress_user,
                       char *err,
                       int err_len);
int games_scan_installed(const XboxConfig *cfg,
                         XboxInstalledGameList *out,
                         char *err,
                         int err_len);
int games_uninstall_installed(const XboxConfig *cfg,
                              const XboxInstalledGame *game,
                              char *err,
                              int err_len);
int games_get_f_drive_space(XboxDriveSpace *out, char *err, int err_len);

#endif // XBOX_GAMES_H
