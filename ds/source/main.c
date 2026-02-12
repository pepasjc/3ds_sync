#include <nds.h>
#include <stdio.h>
#include <fat.h>
#include "common.h"
#include "config.h"
#include "saves.h"
#include "network.h"
#include "ui.h"
#include "update.h"

#define LIST_VISIBLE 20  // Visible titles on screen

static SyncState state;
static int selected = 0;
static int scroll_offset = 0;
static int config_selected = 0;
static bool focus_on_config = false;  // false = saves list, true = config menu

static void update_scroll(void) {
    if (selected < scroll_offset)
        scroll_offset = selected;
    if (selected >= scroll_offset + LIST_VISIBLE)
        scroll_offset = selected - LIST_VISIBLE + 1;
}

int main(int argc, char *argv[]) {
    // argv[0] is the executable path (provided by homebrew loader)
    const char *self_path = (argc > 0 && argv && argv[0]) ? argv[0] : NULL;
    // Initialize FAT first
    if (!fatInitDefault()) {
        consoleDemoInit();
        iprintf("FAT init failed!\n");
        iprintf("Make sure SD/flashcard\nis inserted.\n\n");
        iprintf("Press START to exit\n");
        
        while(pmMainLoop()) {
            swiWaitForVBlank();
            scanKeys();
            if(keysDown() & KEY_START) break;
        }
        return 0;
    }
    
    consoleDemoInit();
    
    // Initialize config
    memset(&state, 0, sizeof(SyncState));
    
    // Load config from same path as 3DS client
    char config_error[256];
    if (!config_load(&state, config_error, sizeof(config_error))) {
        consoleClear();
        iprintf("=== Config Setup ===\n\n");
        iprintf("%s\n\n", config_error);
        iprintf("Press START to exit\n");
        
        while(pmMainLoop()) {
            swiWaitForVBlank();
            scanKeys();
            if(keysDown() & KEY_START) break;
        }
        return 0;
    }
    
    // Initialize network (optional - continue if fails)
    iprintf("Initializing network...\n");
    bool has_wifi = (network_init(&state) == 0);
    if (!has_wifi) {
        iprintf("\nWiFi unavailable\n");
        iprintf("Upload/download disabled\n\n");
        iprintf("Press A to continue\n");
        
        while(pmMainLoop()) {
            swiWaitForVBlank();
            scanKeys();
            if(keysDown() & KEY_A) break;
        }
    }
    
    // Check for pending update before continuing
    if (update_apply_pending(self_path)) {
        iprintf("\nPress START to exit\n");
        while(pmMainLoop()) {
            swiWaitForVBlank();
            scanKeys();
            if(keysDown() & KEY_START) break;
        }
        return 0;
    }
    
    // Scan for saves
    consoleClear();
    iprintf("Scanning saves...\n\n");
    saves_scan(&state);
    
    iprintf("\nFound %d saves!\n", state.num_titles);
    iprintf("\nPress A to continue\n");
    
    while(pmMainLoop()) {
        swiWaitForVBlank();
        scanKeys();
        if(keysDown() & KEY_A) break;
    }
    
    // Set up dual screen mode
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    
    PrintConsole topScreen;
    PrintConsole bottomScreen;
    
    consoleInit(&topScreen, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&bottomScreen, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
    
    if (state.num_titles == 0) {
        consoleSelect(&bottomScreen);
        iprintf("No saves found!\n\n");
        iprintf("Press START to exit\n");
        
        while(pmMainLoop()) {
            swiWaitForVBlank();
            scanKeys();
            if(keysDown() & KEY_START) break;
        }
        return 0;
    }
    
    // Main loop
    bool redraw = true;
    
    while(pmMainLoop()) {
        swiWaitForVBlank();
        scanKeys();
        int pressed = keysDown();
        
        if (pressed & KEY_START)
            break;

        // L button - toggle focus
        if (pressed & KEY_L) {
            focus_on_config = !focus_on_config;
            redraw = true;
        }
        
        if (pressed & KEY_DOWN) {
            if (focus_on_config) {
                config_selected = (config_selected + 1) % 7;
                redraw = true;
            } else if (state.num_titles > 0) {
                selected = (selected + 1) % state.num_titles;
                update_scroll();
                redraw = true;
            }
        }
        
        if (pressed & KEY_UP) {
            if (focus_on_config) {
                config_selected = (config_selected - 1 + 7) % 7;
                redraw = true;
            } else if (state.num_titles > 0) {
                selected = (selected - 1 + state.num_titles) % state.num_titles;
                update_scroll();
                redraw = true;
            }
        }
        
        // Page down with RIGHT (only for saves list)
        if (pressed & KEY_RIGHT && !focus_on_config && state.num_titles > 0) {
            selected += LIST_VISIBLE;
            if (selected >= state.num_titles) selected = state.num_titles - 1;
            update_scroll();
            redraw = true;
        }
        
        // Page up with LEFT (only for saves list)
        if (pressed & KEY_LEFT && !focus_on_config && state.num_titles > 0) {
            selected -= LIST_VISIBLE;
            if (selected < 0) selected = 0;
            update_scroll();
            redraw = true;
        }
        
        // A button - handle config actions or save operations
        if (pressed & KEY_A) {
            if (focus_on_config) {
                // Handle config menu actions
                if (config_selected == 0) {
                    // Edit Server URL
                    if (config_edit_field("http://192.168.1.100:8000", state.server_url, sizeof(state.server_url))) {
                        config_save(&state);
                    }
                    redraw = true;
                } else if (config_selected == 1) {
                    // Edit API Key
                    if (config_edit_field("your-api-key", state.api_key, sizeof(state.api_key))) {
                        config_save(&state);
                    }
                    redraw = true;
                } else if (config_selected == 2) {
                    // Edit WiFi SSID
                    if (config_edit_field("wifi-ssid", state.wifi_ssid, sizeof(state.wifi_ssid))) {
                        config_save(&state);
                    }
                    redraw = true;
                } else if (config_selected == 3) {
                    // Edit WiFi WEP Key
                    if (config_edit_field("wifi-key", state.wifi_wep_key, sizeof(state.wifi_wep_key))) {
                        config_save(&state);
                    }
                    redraw = true;
                } else if (config_selected == 4) {
                    // Rescan Saves
                    consoleSelect(&bottomScreen);
                    consoleClear();
                    iprintf("Rescanning saves...\n\n");
                    saves_scan(&state);
                    selected = 0;
                    scroll_offset = 0;
                    redraw = true;
                } else if (config_selected == 5) {
                    // Connect WiFi
                    consoleSelect(&bottomScreen);
                    consoleClear();
                    iprintf("Connecting WiFi...\n\n");
                    has_wifi = (network_init(&state) == 0);
                    if (!has_wifi) {
                        iprintf("WiFi connection failed\n");
                        iprintf("Press any button\n");
                        while(pmMainLoop()) {
                            swiWaitForVBlank();
                            scanKeys();
                            if(keysDown()) break;
                        }
                    }
                    redraw = true;
                } else if (config_selected == 6) {
                    // Check for updates
                    if (!has_wifi) {
                        consoleSelect(&bottomScreen);
                        consoleClear();
                        iprintf("WiFi required for updates\n");
                        iprintf("Press any button\n");
                        while(pmMainLoop()) {
                            swiWaitForVBlank();
                            scanKeys();
                            if(keysDown()) break;
                        }
                        redraw = true;
                        continue;
                    }
                    
                    consoleSelect(&bottomScreen);
                    consoleClear();
                    iprintf("Checking for updates...\n\n");
                    
                    UpdateInfo update_info;
                    if (!update_check(&state, &update_info)) {
                        iprintf("Update check failed\n");
                        iprintf("Press any button\n");
                        while(pmMainLoop()) {
                            swiWaitForVBlank();
                            scanKeys();
                            if(keysDown()) break;
                        }
                        redraw = true;
                        continue;
                    }
                    
                    if (!update_info.available) {
                        iprintf("You have the latest\n");
                        iprintf("version (%s)\n\n", APP_VERSION);
                        iprintf("Press any button\n");
                        while(pmMainLoop()) {
                            swiWaitForVBlank();
                            scanKeys();
                            if(keysDown()) break;
                        }
                        redraw = true;
                        continue;
                    }
                    
                    // Show update available
                    consoleClear();
                    iprintf("Update available!\n\n");
                    iprintf("Current: %s\n", APP_VERSION);
                    iprintf("Latest:  %s\n\n", update_info.latest_version);
                    iprintf("Size: %zu KB\n\n", update_info.file_size / 1024);
                    iprintf("A: Download & Install\n");
                    iprintf("B: Cancel\n");
                    
                    bool do_update = false;
                    while(pmMainLoop()) {
                        swiWaitForVBlank();
                        scanKeys();
                        int k = keysDown();
                        if (k & KEY_A) { do_update = true; break; }
                        if (k & KEY_B) break;
                    }
                    
                    if (do_update) {
                        consoleClear();
                        iprintf("Downloading...\n\n");
                        
                        if (!update_download(&state, update_info.download_url, NULL)) {
                            iprintf("\nDownload failed\n");
                        } else {
                            iprintf("\nUpdate ready!\n");
                            iprintf("Restart to apply\n");
                        }
                        
                        iprintf("\nPress any button\n");
                        while(pmMainLoop()) {
                            swiWaitForVBlank();
                            scanKeys();
                            if(keysDown()) break;
                        }
                    }
                    redraw = true;
                }
                continue;
            }
        }
        
        // Y button - show save details (only when focused on saves)
        if (pressed & KEY_Y && !focus_on_config && state.num_titles > 0) {
            consoleSelect(&bottomScreen);
            Title *title = &state.titles[selected];
            
            consoleClear();
            iprintf("Loading details...\n");
            
            // Ensure hash is calculated
            if (saves_ensure_hash(title) == 0) {
                ui_show_save_details(title);
            } else {
                iprintf("Failed to calculate hash!\n");
                iprintf("\nPress any button\n");
                
                while(pmMainLoop()) {
                    swiWaitForVBlank();
                    scanKeys();
                    if(keysDown()) break;
                }
            }
            
            redraw = true;
        }
        
        // A button - upload with confirmation (only when focused on saves)
        if (pressed & KEY_A && !focus_on_config && state.num_titles > 0 && has_wifi) {
            consoleSelect(&bottomScreen);
            Title *title = &state.titles[selected];
            
            consoleClear();
            iprintf("Checking server...\n");
            
            // Convert title_id to hex string
            char title_id_hex[17];
            snprintf(title_id_hex, sizeof(title_id_hex), "%02X%02X%02X%02X%02X%02X%02X%02X",
                title->title_id[0], title->title_id[1], title->title_id[2], title->title_id[3],
                title->title_id[4], title->title_id[5], title->title_id[6], title->title_id[7]);
            
            // Clear hash to force fresh calculation
            title->hash_calculated = false;
            
            // Fetch server save info
            char server_hash[65] = "";
            size_t server_size = 0;
            int check_result = network_get_save_info(&state, title_id_hex, server_hash, &server_size);
            
            if (check_result != 0) {
                iprintf("\nFailed to check server!\n");
                iprintf("Press B to go back\n");
                while(pmMainLoop()) {
                    swiWaitForVBlank();
                    scanKeys();
                    if(keysDown() & KEY_B) break;
                }
                redraw = true;
                continue;
            }
            
            // Show confirmation dialog with server info
            if (ui_confirm_sync(title, server_hash, server_size, true)) {
                consoleClear();
                iprintf("Uploading...\n\n");
                
                int result = network_upload(&state, selected);
                if (result == 0) {
                    iprintf("\nUpload successful!\n");
                } else {
                    iprintf("\nUpload failed!\n");
                }
                
                iprintf("Press B to go back\n");
            } else {
                consoleClear();
                iprintf("Upload cancelled\n");
                iprintf("Press B to go back\n");
            }
            
            while(pmMainLoop()) {
                swiWaitForVBlank();
                scanKeys();
                if(keysDown() & KEY_B) break;
            }
            
            redraw = true;
        }
        
        // B button - download with confirmation (only when focused on saves)
        if (pressed & KEY_B && !focus_on_config && state.num_titles > 0 && has_wifi) {
            consoleSelect(&bottomScreen);
            Title *title = &state.titles[selected];
            
            consoleClear();
            iprintf("Checking server...\n");
            
            // Convert title_id to hex string
            char title_id_hex[17];
            snprintf(title_id_hex, sizeof(title_id_hex), "%02X%02X%02X%02X%02X%02X%02X%02X",
                title->title_id[0], title->title_id[1], title->title_id[2], title->title_id[3],
                title->title_id[4], title->title_id[5], title->title_id[6], title->title_id[7]);
            
            // Fetch server save info
            char server_hash[65] = "";
            size_t server_size = 0;
            int has_server = (network_get_save_info(&state, title_id_hex, server_hash, &server_size) == 0);
            
            if (!has_server) {
                iprintf("\nSave not found on server!\n");
                iprintf("Press B to go back\n");
                while(pmMainLoop()) {
                    swiWaitForVBlank();
                    scanKeys();
                    if(keysDown() & KEY_B) break;
                }
                redraw = true;
                continue;
            }
            
            // Clear hash flag so we recalculate from current save file
            title->hash_calculated = false;
            
            // Show confirmation dialog with server info
            if (ui_confirm_sync(title, server_hash, server_size, false)) {
                consoleClear();
                iprintf("Downloading...\n\n");
                
                int result = network_download(&state, selected);
                if (result == 0) {
                    iprintf("\nDownload successful!\n");
                } else {
                    iprintf("\nDownload failed!\n");
                }
                
                iprintf("Press B to go back\n");
            } else {
                consoleClear();
                iprintf("Download cancelled\n");
                iprintf("Press B to go back\n");
            }
            
            while(pmMainLoop()) {
                swiWaitForVBlank();
                scanKeys();
                if(keysDown() & KEY_B) break;
            }
            
            redraw = true;
        }
        
        if (redraw) {
            // Draw config on top screen
            consoleSelect(&topScreen);
            consoleClear();
            ui_draw_config(&state, config_selected, focus_on_config, has_wifi);
            
            // Draw saves list on bottom screen
            consoleSelect(&bottomScreen);
            consoleClear();
            iprintf("=== NDS Save Sync v%s ===\n", APP_VERSION);
            iprintf("Found %d saves\n\n", state.num_titles);
            
            // Display visible titles
            int start = scroll_offset;
            int end = (scroll_offset + LIST_VISIBLE < state.num_titles) ? 
                      scroll_offset + LIST_VISIBLE : state.num_titles;
            
            for (int i = start; i < end; i++) {
                if (i == selected) {
                    iprintf("> ");
                } else {
                    iprintf("  ");
                }
                
                // Truncate long names
                char name[25];
                strncpy(name, state.titles[i].game_name, 24);
                name[24] = '\0';
                
                // Show server status indicator
                char status = state.titles[i].on_server ? 'S' : ' ';
                iprintf("%-24s [%c]\n", name, status);
            }
            
            
            redraw = false;
        }
    }
    
    return 0;
}
