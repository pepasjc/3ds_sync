#include "saves.h"
#include "sha256.h"
#include <dirent.h>
#include <sys/stat.h>
#include <fat.h>

// nds-bootstrap save paths
#define BOOTSTRAP_SAVES_PATH "sd:/roms/nds/saves"
#define MAX_PATH_LEN 256
#define NDS_GAMECODE_OFFSET 0x0C

// Compare titles by name for sorting (case-insensitive)
static int title_compare(const void *a, const void *b) {
    return strcasecmp(((const Title *)a)->game_name, ((const Title *)b)->game_name);
}

// Read product code from NDS ROM header
// Returns true if successful, fills code_out with 4-char code
static bool read_rom_gamecode(const char *rom_path, char *code_out) {
    FILE *f = fopen(rom_path, "rb");
    if (!f) return false;
    
    fseek(f, NDS_GAMECODE_OFFSET, SEEK_SET);
    char code[4];
    size_t rd = fread(code, 1, 4, f);
    fclose(f);
    
    if (rd != 4) return false;
    
    // Validate printable ASCII
    for (int i = 0; i < 4; i++) {
        if (code[i] < 0x20 || code[i] > 0x7E)
            return false;
    }
    
    memcpy(code_out, code, 4);
    code_out[4] = '\0';
    return true;
}

// Find corresponding ROM file for a save file
// Tries: basename.nds, basename (no ext).nds, etc.
static bool find_rom_for_save(const char *save_path, char *rom_path_out, size_t rom_path_size) {
    char base[MAX_PATH_LEN];
    strncpy(base, save_path, sizeof(base) - 1);
    
    // Remove .sav extension
    char *ext = strrchr(base, '.');
    if (ext) *ext = '\0';
    
    // Try: basename.nds
    snprintf(rom_path_out, rom_path_size, "%s.nds", base);
    struct stat st;
    if (stat(rom_path_out, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    
    // Try: basename.NDS
    snprintf(rom_path_out, rom_path_size, "%s.NDS", base);
    if (stat(rom_path_out, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    
    return false;
}

// Find corresponding save file for a ROM file
// Tries: basename.sav, saves/basename.sav
static bool find_sav_for_rom(const char *rom_path, char *sav_path_out, size_t sav_path_size) {
    char base[MAX_PATH_LEN];
    strncpy(base, rom_path, sizeof(base) - 1);
    
    // Remove .nds extension
    char *ext = strrchr(base, '.');
    if (ext) *ext = '\0';
    
    // Try: basename.sav
    snprintf(sav_path_out, sav_path_size, "%s.sav", base);
    struct stat st;
    if (stat(sav_path_out, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    
    // Try: basename.SAV
    snprintf(sav_path_out, sav_path_size, "%s.SAV", base);
    if (stat(sav_path_out, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    
    // Try saves/ subfolder
    char dir[MAX_PATH_LEN];
    strncpy(dir, rom_path, sizeof(dir) - 1);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        char *filename = last_slash + 1;
        char stem[MAX_PATH_LEN];
        strncpy(stem, filename, sizeof(stem) - 1);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = '\0';
        *last_slash = '\0';
        
        snprintf(sav_path_out, sav_path_size, "%s/saves/%s.sav", dir, stem);
        if (stat(sav_path_out, &st) == 0 && S_ISREG(st.st_mode)) {
            return true;
        }
    }
    
    return false;
}

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

// Scan for .nds ROM files in flashcard directory
static int scan_flashcard_roms(SyncState *state) {
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
        int files_in_dir = 0;
        while ((ent = readdir(dir)) != NULL && count < MAX_TITLES) {
            files_in_dir++;
            
            // Skip . and ..
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            
            // Look for .nds ROM files
            char *ext = strrchr(ent->d_name, '.');
            if (ext && (strcasecmp(ext, ".nds") == 0 || strcasecmp(ext, ".NDS") == 0)) {
                iprintf("Found ROM: %s\n", ent->d_name);
                snprintf(path, MAX_PATH_LEN, "%s%s", paths[i], ent->d_name);
                
                struct stat st;
                if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                    // Read product code from ROM header
                    char product_code[5];
                    if (!read_rom_gamecode(path, product_code)) {
                        continue;  // Skip if can't read product code
                    }
                    
                    // Check for duplicate product codes (already scanned from different path)
                    bool is_duplicate = false;
                    for (int j = 0; j < count; j++) {
                        // Compare product codes (title_id bytes 4-7)
                        if (memcmp(&state->titles[j].title_id[4], product_code, 4) == 0) {
                            is_duplicate = true;
                            iprintf("  Duplicate (already added)\n");
                            break;
                        }
                    }
                    if (is_duplicate) continue;
                    
                    Title *title = &state->titles[count];
                    
                    // Extract game name from filename (remove .nds)
                    strncpy(title->game_name, ent->d_name, sizeof(title->game_name) - 1);
                    char *dot = strrchr(title->game_name, '.');
                    if (dot) *dot = '\0';
                    
                    // Generate title_id from product code
                    title->title_id[0] = 0x00;
                    title->title_id[1] = 0x04;
                    title->title_id[2] = 0x80;
                    title->title_id[3] = 0x00;
                    for (int i = 0; i < 4; i++) {
                        title->title_id[4 + i] = (uint8_t)product_code[i];
                    }
                    
                    // Find corresponding save file
                    char sav_path[MAX_PATH_LEN];
                    if (find_sav_for_rom(path, sav_path, sizeof(sav_path))) {
                        // Save exists, get its size
                        struct stat sav_st;
                        if (stat(sav_path, &sav_st) == 0 && S_ISREG(sav_st.st_mode)) {
                            title->save_size = sav_st.st_size;
                            strncpy(title->save_path, sav_path, sizeof(title->save_path) - 1);
                        }
                    } else {
                        // No save yet, use default path for future download
                        snprintf(sav_path, sizeof(sav_path), "%s", path);
                        char *dot = strrchr(sav_path, '.');
                        if (dot) strcpy(dot, ".sav");
                        title->save_size = 0;
                        strncpy(title->save_path, sav_path, sizeof(title->save_path) - 1);
                    }
                    
                    title->is_cartridge = 0;
                    title->hash_calculated = false;
                    
                    count++;
                }
            }
        }
        iprintf("  Files: %d, Added: %d\n", files_in_dir, count);
        closedir(dir);
    }
    
    return count;
}

// Scan nds-bootstrap ROM directory (ROMs are in sd:/roms/nds, saves in sd:/roms/nds/saves)
static int scan_bootstrap_roms(SyncState *state, const char *saves_path) {
    DIR *dir;
    struct dirent *ent;
    char path[MAX_PATH_LEN];
    int count = 0;
    
    // Extract parent directory (remove /saves suffix to get ROM directory)
    char rom_dir[MAX_PATH_LEN];
    strncpy(rom_dir, saves_path, sizeof(rom_dir) - 1);
    rom_dir[sizeof(rom_dir) - 1] = '\0';
    char *last_slash = strrchr(rom_dir, '/');
    if (last_slash) {
        *last_slash = '\0';  // Now rom_dir is "sd:/roms/nds"
    } else {
        return 0;  // Invalid path
    }
    
    iprintf("ROM dir: %s\n", rom_dir);
    
    dir = opendir(rom_dir);
    if (!dir) {
        iprintf("Failed to open ROM dir\n");
        return 0;
    }
    
    while ((ent = readdir(dir)) != NULL && count < MAX_TITLES) {
        // Skip . and ..
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        
        // Look for .nds ROM files
        char *ext = strrchr(ent->d_name, '.');
        if (ext && (strcasecmp(ext, ".nds") == 0 || strcasecmp(ext, ".NDS") == 0)) {
            char rom_path[MAX_PATH_LEN];
            snprintf(rom_path, MAX_PATH_LEN, "%s/%s", rom_dir, ent->d_name);
            
            struct stat st;
            if (stat(rom_path, &st) == 0 && S_ISREG(st.st_mode)) {
                // Read product code from ROM header
                char product_code[5];
                if (!read_rom_gamecode(rom_path, product_code)) {
                    continue;  // Skip if can't read product code
                }
                
                Title *title = &state->titles[count];
                
                // Extract game name from filename (remove .nds)
                strncpy(title->game_name, ent->d_name, sizeof(title->game_name) - 1);
                char *dot = strrchr(title->game_name, '.');
                if (dot) *dot = '\0';
                
                // Generate title_id from product code
                title->title_id[0] = 0x00;
                title->title_id[1] = 0x04;
                title->title_id[2] = 0x80;
                title->title_id[3] = 0x00;
                for (int i = 0; i < 4; i++) {
                    title->title_id[4 + i] = (uint8_t)product_code[i];
                }
                
                // Look for save file in saves directory
                char sav_name[MAX_PATH_LEN];
                strncpy(sav_name, ent->d_name, sizeof(sav_name) - 1);
                char *dot2 = strrchr(sav_name, '.');
                if (dot2) strcpy(dot2, ".sav");
                
                char sav_path[MAX_PATH_LEN];
                snprintf(sav_path, sizeof(sav_path), "%s/%s", saves_path, sav_name);
                
                struct stat sav_st;
                if (stat(sav_path, &sav_st) == 0 && S_ISREG(sav_st.st_mode)) {
                    // Save exists
                    title->save_size = sav_st.st_size;
                    strncpy(title->save_path, sav_path, sizeof(title->save_path) - 1);
                } else {
                    // No save yet
                    title->save_size = 0;
                    strncpy(title->save_path, sav_path, sizeof(title->save_path) - 1);
                }
                
                title->is_cartridge = 0;
                title->hash_calculated = false;
                
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
        state->num_titles = scan_bootstrap_roms(state, "sd:/roms/nds/saves");
        iprintf("Bootstrap: %d saves\n", state->num_titles);
    } else if (bootstrap_mode == 2) {
        iprintf("Scanning sdmc:/roms/nds/saves\n");
        state->num_titles = scan_bootstrap_roms(state, "sdmc:/roms/nds/saves");
        iprintf("Bootstrap: %d saves\n", state->num_titles);
    } else {
        iprintf("Scanning flashcard paths\n");
        state->num_titles = scan_flashcard_roms(state);
        iprintf("Flashcard: %d saves\n", state->num_titles);
    }
    
    // Sort titles alphabetically by name
    if (state->num_titles > 0) {
        qsort(state->titles, state->num_titles, sizeof(Title), title_compare);
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
