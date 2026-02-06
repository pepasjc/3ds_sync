#ifndef COMMON_H
#define COMMON_H

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_VERSION "0.1.0"

// Max values
#define MAX_TITLES        256
#define MAX_PATH_LEN      256
#define MAX_URL_LEN       256
#define MAX_API_KEY_LEN   128

// Config file location on SD card
#define CONFIG_PATH "sdmc:/3ds/3dssync/config.txt"
#define BACKUP_DIR  "sdmc:/3ds/3dssync/backups"

// Title info for display and sync
typedef struct {
    u64 title_id;
    FS_MediaType media_type;
    char title_id_hex[17]; // 16 hex chars + null
    bool has_save_data;
} TitleInfo;

// App configuration loaded from SD card
typedef struct {
    char server_url[MAX_URL_LEN];
    char api_key[MAX_API_KEY_LEN];
} AppConfig;

#endif // COMMON_H
