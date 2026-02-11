#include <nds.h>
#include <stdio.h>
#include <fat.h>
#include "common.h"
#include "config.h"
#include "saves.h"
#include "network.h"
#include "ui.h"

#define LIST_VISIBLE 20  // Visible titles on screen

static SyncState state;
static int selected = 0;
static int scroll_offset = 0;

static void update_scroll(void) {
    if (selected < scroll_offset)
        scroll_offset = selected;
    if (selected >= scroll_offset + LIST_VISIBLE)
        scroll_offset = selected - LIST_VISIBLE + 1;
}

int main(void) {
    consoleDemoInit();
    
    // Initialize FAT
    if (!fatInitDefault()) {
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
    
    // Scan for saves
    consoleClear();
    iprintf("Scanning saves...\n\n");
    saves_scan(&state);
    
    iprintf("\nFound %d saves!\n", state.num_titles);
    
    // TODO: Fetch server save list if WiFi available
    // Currently disabled due to large response size (78KB+ JSON)
    // Need to implement chunked reading in http.c first
    // if (has_wifi) {
    //     iprintf("Fetching server list...\n");
    //     network_fetch_saves(&state);
    // }
    
    iprintf("\nPress A to continue\n");
    
    while(pmMainLoop()) {
        swiWaitForVBlank();
        scanKeys();
        if(keysDown() & KEY_A) break;
    }
    
    // Draw initial screen
    consoleClear();
    iprintf("=== NDS Save Sync ===\n");
    iprintf("Found %d saves\n\n", state.num_titles);
    
    if (state.num_titles == 0) {
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
        
        if (pressed & KEY_DOWN && state.num_titles > 0) {
            selected = (selected + 1) % state.num_titles;
            update_scroll();
            redraw = true;
        }
        
        if (pressed & KEY_UP && state.num_titles > 0) {
            selected = (selected - 1 + state.num_titles) % state.num_titles;
            update_scroll();
            redraw = true;
        }
        
        // Page down with RIGHT
        if (pressed & KEY_RIGHT && state.num_titles > 0) {
            selected += LIST_VISIBLE;
            if (selected >= state.num_titles) selected = state.num_titles - 1;
            update_scroll();
            redraw = true;
        }
        
        // Page up with LEFT
        if (pressed & KEY_LEFT && state.num_titles > 0) {
            selected -= LIST_VISIBLE;
            if (selected < 0) selected = 0;
            update_scroll();
            redraw = true;
        }
        
        // Y button - show save details
        if (pressed & KEY_Y && state.num_titles > 0) {
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
        
        // A button - upload with confirmation
        if (pressed & KEY_A && state.num_titles > 0 && has_wifi) {
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
        
        // B button - download with confirmation
        if (pressed & KEY_B && state.num_titles > 0 && has_wifi) {
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
            consoleClear();
            iprintf("=== NDS Save Sync ===\n");
            iprintf("Found %d saves | UP/DN:Nav Y:Info\n\n", state.num_titles);
            
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
            
            iprintf("\n");
            if (has_wifi) {
                iprintf("A:Upload B:Download START:Exit\n");
            } else {
                iprintf("No WiFi - START:Exit\n");
            }
            
            redraw = false;
        }
    }
    
    return 0;
}
