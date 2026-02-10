#include "saves.h"
#include "sha256.h"
#include <dirent.h>
#include <sys/stat.h>
#include <fat.h>

// nds-bootstrap save paths
#define BOOTSTRAP_SAVES_PATH "sd:/roms/nds/saves"
#define MAX_PATH_LEN 256

// Check if running from nds-bootstrap (saves on SD) or flashcard
// Returns: 0=flashcard, 1=bootstrap with sd:, 2=bootstrap with sdmc:
static int is_nds_bootstrap() {
    struct stat st;
    
    // Try sd: prefix
    if (stat("sd:/roms/nds/saves", &st) == 0 && S_ISDIR(st.st_mode)) {
        return 1;
    }
    
    // Try sdmc: prefix
    if (stat("sdmc:/roms/nds/saves", &st) == 0 && S_ISDIR(st.st_mode)) {
        return 2;
    }
    
    return 0;
}

// Scan for .sav files in flashcard directory
static int scan_flashcard_saves(SyncState *state) {
    DIR *dir;
    struct dirent *ent;
    char path[MAX_PATH_LEN];
    int count = 0;
    
    // Default flashcard paths
    const char *paths[] = {
        "fat:/roms/",
        "/roms/",
        "sd:/roms/",
        "fat:/",
        NULL
    };
    
    // Scan default paths
    for (int i = 0; paths[i] && count < MAX_TITLES; i++) {
        iprintf("Trying: %s\n", paths[i]);
        dir = opendir(paths[i]);
        if (!dir) {
            iprintf("  Failed\n");
            continue;
        }
        
        iprintf("  OK!\n");
        while ((ent = readdir(dir)) != NULL && count < MAX_TITLES) {
            // Skip . and ..
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            
            // Look for .sav files
            char *ext = strrchr(ent->d_name, '.');
            if (ext && (strcasecmp(ext, ".sav") == 0 || strcasecmp(ext, ".SAV") == 0)) {
                snprintf(path, MAX_PATH_LEN, "%s%s", paths[i], ent->d_name);
                
                struct stat st;
                if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                    Title *title = &state->titles[count];
                    
                    // Extract game name from filename (remove .sav)
                    strncpy(title->game_name, ent->d_name, sizeof(title->game_name) - 1);
                    char *dot = strrchr(title->game_name, '.');
                    if (dot) *dot = '\0';
                    
                    title->save_size = st.st_size;
                    title->is_cartridge = 0;
                    title->hash_calculated = false;  // Don't calculate hash yet
                    strncpy(title->save_path, path, sizeof(title->save_path) - 1);
                    
                    count++;
                }
            }
        }
        closedir(dir);
    }
    
    return count;
}

// Scan nds-bootstrap saves directory
static int scan_bootstrap_saves(SyncState *state, const char *saves_path) {
    DIR *dir;
    struct dirent *ent;
    char path[MAX_PATH_LEN];
    int count = 0;
    
    dir = opendir(saves_path);
    if (!dir) return 0;
    
    while ((ent = readdir(dir)) != NULL && count < MAX_TITLES) {
        // Skip . and ..
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        
        // Check if this is a .sav file (type 8 = DT_REG = regular file)
        char *ext = strrchr(ent->d_name, '.');
        if (ext && (strcasecmp(ext, ".sav") == 0 || strcasecmp(ext, ".SAV") == 0)) {
            char savepath[MAX_PATH_LEN];
            snprintf(savepath, MAX_PATH_LEN, "%s/%s", saves_path, ent->d_name);
            
            struct stat st;
            if (stat(savepath, &st) == 0 && S_ISREG(st.st_mode)) {
                Title *title = &state->titles[count];
                
                // Extract game name from filename (remove .sav)
                strncpy(title->game_name, ent->d_name, sizeof(title->game_name) - 1);
                char *dot = strrchr(title->game_name, '.');
                if (dot) *dot = '\0';
                
                title->save_size = st.st_size;
                title->is_cartridge = 0;
                title->hash_calculated = false;  // Don't calculate hash yet
                strncpy(title->save_path, savepath, sizeof(title->save_path) - 1);
                
                count++;
            }
        }
        
        // Also check for subdirectories with TID structure (original logic)
        if (ent->d_type == DT_DIR && strlen(ent->d_name) == 16) {
            // This looks like a title ID (16 hex chars)
            snprintf(path, MAX_PATH_LEN, "%s/%s", saves_path, ent->d_name);
            
            // Look for save files in this directory
            DIR *savedir = opendir(path);
            if (savedir) {
                struct dirent *saveent;
                while ((saveent = readdir(savedir)) != NULL && count < MAX_TITLES) {
                    if (strstr(saveent->d_name, ".sav") || strstr(saveent->d_name, ".SAV")) {
                        char savepath[MAX_PATH_LEN];
                        snprintf(savepath, MAX_PATH_LEN, "%s/%s", path, saveent->d_name);
                        
                        struct stat st;
                        if (stat(savepath, &st) == 0 && S_ISREG(st.st_mode)) {
                            Title *title = &state->titles[count];
                            
                            // Use TID as game name for now (can lookup later)
                            strncpy(title->game_name, ent->d_name, sizeof(title->game_name) - 1);
                            title->save_size = st.st_size;
                            title->is_cartridge = 0;
                            title->hash_calculated = false;  // Don't calculate hash yet
                            strncpy(title->save_path, savepath, sizeof(title->save_path) - 1);
                            
                            count++;
                        }
                    }
                }
                closedir(savedir);
            }
        }
    }
    
    closedir(dir);
    
    return count;
}

int saves_scan(SyncState *state) {
    state->num_titles = 0;
    
    // Check which type of setup we're running from
    int bootstrap_mode = is_nds_bootstrap();
    
    iprintf("Bootstrap mode: %d\n", bootstrap_mode);
    
    if (bootstrap_mode == 1) {
        iprintf("Scanning sd:/roms/nds/saves\n");
        state->num_titles = scan_bootstrap_saves(state, "sd:/roms/nds/saves");
        iprintf("Bootstrap: %d saves\n", state->num_titles);
    } else if (bootstrap_mode == 2) {
        iprintf("Scanning sdmc:/roms/nds/saves\n");
        state->num_titles = scan_bootstrap_saves(state, "sdmc:/roms/nds/saves");
        iprintf("Bootstrap: %d saves\n", state->num_titles);
    } else {
        iprintf("Scanning flashcard paths\n");
        state->num_titles = scan_flashcard_saves(state);
        iprintf("Flashcard: %d saves\n", state->num_titles);
    }
    
    return 0;
}

int saves_compute_hash(const char *path, uint8_t *hash) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // Allocate buffer
    uint8_t *buffer = (uint8_t*)malloc(size);
    if (!buffer) {
        fclose(f);
        return -1;
    }
    
    // Read file
    size_t read = fread(buffer, 1, size, f);
    fclose(f);
    
    if (read != size) {
        free(buffer);
        return -1;
    }
    
    // Calculate SHA-256
    sha256_hash(buffer, size, hash);
    free(buffer);
    
    return 0;
}

// Compute hash for a title if not already calculated
int saves_ensure_hash(Title *title) {
    if (title->hash_calculated) {
        return 0;  // Already calculated
    }
    
    if (saves_compute_hash(title->save_path, title->hash) == 0) {
        title->hash_calculated = true;
        return 0;
    }
    
    return -1;  // Failed to compute
}

int saves_read_cartridge(uint8_t *buffer, uint32_t *size) {
    // TODO: Read save from DS cartridge via GBA Slot or native slot
    // DS cartridge save types:
    // - EEPROM (4KB, 64KB)
    // - SRAM (256KB)
    // - Flash (256KB, 512KB, 1MB)
    // - FRAM (256KB)
    
    printf("Read cartridge: Not yet implemented\n");
    return -1;
}

int saves_write_cartridge(const uint8_t *buffer, uint32_t size) {
    // TODO: Write save to DS cartridge
    printf("Write cartridge: Not yet implemented\n");
    return -1;
}
