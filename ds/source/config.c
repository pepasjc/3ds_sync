#include "config.h"
#include <stdio.h>
#include <string.h>
#include <fat.h>
#include <sys/stat.h>
#include <sys/types.h>

// Try to create directory if it doesn't exist
static void ensure_directory(const char *path) {
    mkdir(path, 0777);
}

// Create default config file
static bool create_default_config(const char *path) {
    // Extract directory from path
    char dir[256];
    strncpy(dir, path, sizeof(dir) - 1);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        ensure_directory(dir);
    }
    
    FILE *f = fopen(path, "w");
    if (!f) return false;
    
    fprintf(f, "# NDS/3DS Save Sync Configuration\n");
    fprintf(f, "# Edit the values below with your server details\n\n");
    fprintf(f, "server_url=http://192.168.1.100:8000\n");
    fprintf(f, "api_key=change-this-to-your-api-key\n\n");
    fprintf(f, "# WiFi Configuration (for Nintendo DS/DS Lite)\n");
    fprintf(f, "# Leave blank to skip WiFi or use DSi firmware settings\n");
    fprintf(f, "wifi_ssid=\n");
    fprintf(f, "wifi_wep_key=\n\n");
    fprintf(f, "# Optional: Custom save directory to scan (in addition to defaults)\n");
    fprintf(f, "# Examples: /data/saves, sd:/nds/saves, fat:/saves\n");
    fprintf(f, "#save_dir=/your/custom/path\n");
    
    fclose(f);
    return true;
}

// Try to load config from multiple possible paths
bool config_load(SyncState *state, char *error, size_t error_size) {
    const char *paths[] = {
        "sd:/dssync/config.txt",
        "fat:/dssync/config.txt",
        "/dssync/config.txt",
        "sdmc:/dssync/config.txt",
        NULL
    };
    
    FILE *f = NULL;
    const char *loaded_path = NULL;
    const char *tried_create = NULL;
    
    // Try each path
    for (int i = 0; paths[i]; i++) {
        f = fopen(paths[i], "r");
        if (f) {
            loaded_path = paths[i];
            break;
        }
    }
    
    // If no config found, try to create one
    if (!f) {
        for (int i = 0; paths[i]; i++) {
            if (create_default_config(paths[i])) {
                tried_create = paths[i];
                f = fopen(paths[i], "r");
                if (f) {
                    loaded_path = paths[i];
                    break;
                }
            }
        }
    }
    
    if (!f) {
        snprintf(error, error_size,
            "Could not create config file.\n\n"
            "Please create manually at:\n"
            "sd:/dssync/config.txt\n\n"
            "With contents:\n"
            "server_url=http://<ip>:8000\n"
            "api_key=<your-key>");
        return false;
    }
    
    // Parse config file
    char line[512];
    bool has_url = false;
    bool has_key = false;
    bool is_default = false;
    
    // Initialize fields
    state->custom_save_dir[0] = '\0';
    state->wifi_ssid[0] = '\0';
    state->wifi_wep_key[0] = '\0';
    
    while (fgets(line, sizeof(line), f)) {
        // Remove trailing newline
        char *newline = strchr(line, '\n');
        if (newline) *newline = '\0';
        
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\0') continue;
        
        // Parse key=value
        char *equals = strchr(line, '=');
        if (!equals) continue;
        
        *equals = '\0';
        char *key = line;
        char *value = equals + 1;
        
        if (strcmp(key, "server_url") == 0) {
            strncpy(state->server_url, value, sizeof(state->server_url) - 1);
            has_url = true;
            if (strstr(value, "192.168.1.100")) {
                is_default = true;
            }
        } else if (strcmp(key, "api_key") == 0) {
            strncpy(state->api_key, value, sizeof(state->api_key) - 1);
            has_key = true;
            if (strstr(value, "change-this")) {
                is_default = true;
            }
        } else if (strcmp(key, "save_dir") == 0) {
            strncpy(state->custom_save_dir, value, sizeof(state->custom_save_dir) - 1);
        } else if (strcmp(key, "wifi_ssid") == 0) {
            strncpy(state->wifi_ssid, value, sizeof(state->wifi_ssid) - 1);
        } else if (strcmp(key, "wifi_wep_key") == 0) {
            strncpy(state->wifi_wep_key, value, sizeof(state->wifi_wep_key) - 1);
        }
    }
    
    fclose(f);
    
    if (!has_url || !has_key) {
        snprintf(error, error_size, "Config missing server_url or api_key");
        return false;
    }
    
    if (is_default) {
        snprintf(error, error_size,
            "Config created at:\n%s\n\n"
            "Please edit it with your\n"
            "server IP and API key.",
            tried_create ? tried_create : loaded_path);
        return false;
    }
    
    // Generate console ID
    state->console_id = 0x4E445300;  // "NDS\0"
    
    return true;
}
