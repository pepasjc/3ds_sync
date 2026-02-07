#include "ui.h"
#include "sync.h"

static PrintConsole top_screen;
static PrintConsole bottom_screen;

#define TOP_ROWS    30
#define TOP_COLS    50
#define LIST_ROWS   (TOP_ROWS - 3) // Reserve top 2 for header, bottom 1 for count

void ui_init(void) {
    consoleInit(GFX_TOP, &top_screen);
    consoleInit(GFX_BOTTOM, &bottom_screen);
}

void ui_reinit(void) {
    consoleInit(GFX_TOP, &top_screen);
    consoleInit(GFX_BOTTOM, &bottom_screen);
}

static const char *media_type_str(FS_MediaType mt) {
    switch (mt) {
        case MEDIATYPE_SD:        return "SD";
        case MEDIATYPE_GAME_CARD: return "Card";
        default:                  return "?";
    }
}

void ui_draw_title_list(const TitleInfo *titles, int count, int selected, int scroll_offset) {
    consoleSelect(&top_screen);

    // Header (line 1) - pad to full width to overwrite without clearing
    printf("\x1b[1;1H\x1b[36m--- 3DS Save Sync v%s ---\x1b[0m%-*s",
        APP_VERSION, TOP_COLS - 24, "");

    if (count == 0) {
        printf("\x1b[3;1H  No titles with save data found.%-*s", TOP_COLS - 34, "");
        printf("\x1b[4;1H  Make sure you have games installed.%-*s", TOP_COLS - 38, "");
        // Blank remaining lines with spaces
        for (int i = 5; i <= TOP_ROWS; i++) {
            printf("\x1b[%d;1H%-*s", i, TOP_COLS, "");
        }
        return;
    }

    // Title list (scrollable) - starts at line 3
    for (int i = 0; i < LIST_ROWS; i++) {
        int row = 3 + i;  // Start at line 3
        int idx = scroll_offset + i;

        printf("\x1b[%d;1H", row);  // Position cursor

        if (idx >= count) {
            // Blank line - overwrite with spaces
            printf("%-*s", TOP_COLS, "");
            continue;
        }

        const TitleInfo *t = &titles[idx];
        const char *cursor = (idx == selected) ? ">" : " ";

        // Color: red for conflict, cyan for cartridge, yellow for selected, white otherwise
        const char *color;
        if (t->in_conflict) {
            color = "\x1b[31m";  // Red for conflict
        } else if (t->media_type == MEDIATYPE_GAME_CARD) {
            color = "\x1b[36m";  // Cyan for cartridge (manual sync only)
        } else if (idx == selected) {
            color = "\x1b[33m";  // Yellow for selected
        } else {
            color = "\x1b[0m";
        }

        // Format line and pad to full width
        char line[TOP_COLS + 1];
        snprintf(line, sizeof(line), " %s %-4s %.42s",
            cursor,
            media_type_str(t->media_type),
            t->name);

        printf("%s%-*s\x1b[0m", color, TOP_COLS, line);
    }

    // Footer with count (last row) - pad to full width
    char footer[TOP_COLS + 1];
    snprintf(footer, sizeof(footer), " %d title(s) | D-Pad: navigate", count);
    printf("\x1b[%d;1H\x1b[90m%-*s\x1b[0m", TOP_ROWS, TOP_COLS, footer);
}

#define BOT_COLS 40  // Bottom screen width

void ui_draw_status(const char *status_line) {
    consoleSelect(&bottom_screen);

    // Overwrite each line - pad to full width instead of clearing
    printf("\x1b[1;1H\x1b[36mActions:\x1b[0m%-*s", BOT_COLS - 8, "");
    printf("\x1b[2;1H A - Upload | B - Download%-*s", BOT_COLS - 26, "");
    printf("\x1b[3;1H X - Sync all (SD only)%-*s", BOT_COLS - 23, "");
    printf("\x1b[4;1H Y - Rescan titles%-*s", BOT_COLS - 18, "");
    printf("\x1b[5;1H L - Config | R - Save details%-*s", BOT_COLS - 30, "");
    printf("\x1b[6;1H SELECT - Updates%-*s", BOT_COLS - 17, "");
    printf("\x1b[7;1H START - Exit%-*s", BOT_COLS - 13, "");
    printf("\x1b[8;1H%-*s", BOT_COLS, "");
    printf("\x1b[9;1H\x1b[36mCyan\x1b[0m = cartridge (A/B only)%-*s", BOT_COLS - 27, "");
    printf("\x1b[10;1H%-*s", BOT_COLS, "");
    printf("\x1b[11;1H%-*s", BOT_COLS, "");

    char status_padded[BOT_COLS + 1];
    snprintf(status_padded, sizeof(status_padded), "%s", status_line ? status_line : "Ready.");
    printf("\x1b[12;1H\x1b[90m%-*s\x1b[0m", BOT_COLS, status_padded);
}

void ui_draw_message(const char *msg) {
    consoleSelect(&bottom_screen);
    consoleClear();
    printf("\x1b[1;1H%s\n", msg);
}

void ui_update_progress(const char *msg) {
    // Lightweight update: just overwrite line 1, pad to full width
    consoleSelect(&bottom_screen);
    printf("\x1b[1;1H%-*s", BOT_COLS, msg);
}

void ui_clear(void) {
    consoleSelect(&top_screen);
    consoleClear();
    consoleSelect(&bottom_screen);
    consoleClear();
}

// Format size in human-readable form
static void format_size(u32 bytes, char *out, int out_size) {
    if (bytes >= 1024 * 1024) {
        snprintf(out, out_size, "%.1f MB", bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(out, out_size, "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(out, out_size, "%lu B", (unsigned long)bytes);
    }
}

// Format date from ISO 8601 (YYYY-MM-DDTHH:MM:SS) to readable form
static void format_date(const char *iso, char *out, int out_size) {
    // Extract date and time parts
    if (strlen(iso) >= 16 && iso[10] == 'T') {
        snprintf(out, out_size, "%.10s %.5s", iso, iso + 11);
    } else if (strlen(iso) > 0) {
        snprintf(out, out_size, "%.19s", iso);
    } else {
        snprintf(out, out_size, "N/A");
    }
}

void ui_show_save_details(const TitleInfo *title, const SaveDetails *details) {
    consoleSelect(&top_screen);
    consoleClear();

    int row = 1;

    // Title name header (truncate if too long)
    printf("\x1b[%d;1H\x1b[36m--- %.44s ---\x1b[0m", row++, title->name);
    row++;

    // Title ID
    printf("\x1b[%d;1H Title ID: %s", row++, title->title_id_hex);

    // Media type
    const char *media = (title->media_type == MEDIATYPE_SD) ? "SD Card" :
                        (title->media_type == MEDIATYPE_GAME_CARD) ? "Game Card" : "Unknown";
    printf("\x1b[%d;1H Media:    %s", row++, media);
    row++;

    // Local save info
    printf("\x1b[%d;1H\x1b[33m-- Local Save --\x1b[0m", row++);
    if (details->local_exists) {
        char size_str[32];
        format_size(details->local_size, size_str, sizeof(size_str));
        printf("\x1b[%d;1H Files: %d | Size: %s", row++, details->local_file_count, size_str);
        printf("\x1b[%d;1H Hash:  %.32s...", row++, details->local_hash);
    } else {
        printf("\x1b[%d;1H No local save data", row++);
    }
    row++;

    // Server save info
    printf("\x1b[%d;1H\x1b[33m-- Server Save --\x1b[0m", row++);
    if (details->server_exists) {
        char size_str[32];
        format_size(details->server_size, size_str, sizeof(size_str));
        printf("\x1b[%d;1H Files: %d | Size: %s", row++, details->server_file_count, size_str);
        printf("\x1b[%d;1H Hash:  %.32s...", row++, details->server_hash);

        char date_str[32];
        format_date(details->server_last_sync, date_str, sizeof(date_str));
        printf("\x1b[%d;1H Last sync: %s", row++, date_str);

        if (details->server_console_id[0]) {
            printf("\x1b[%d;1H From console: %.16s", row++, details->server_console_id);
        }
    } else {
        printf("\x1b[%d;1H Not yet uploaded to server", row++);
    }
    row++;

    // Sync status
    printf("\x1b[%d;1H\x1b[33m-- Sync Status --\x1b[0m", row++);
    if (details->is_synced) {
        printf("\x1b[%d;1H\x1b[32m Synced (hashes match)\x1b[0m", row++);
    } else if (details->local_exists && details->server_exists) {
        printf("\x1b[%d;1H\x1b[31m Out of sync (different hashes)\x1b[0m", row++);
    } else if (details->local_exists && !details->server_exists) {
        printf("\x1b[%d;1H\x1b[33m Local only (not uploaded)\x1b[0m", row++);
    } else if (!details->local_exists && details->server_exists) {
        printf("\x1b[%d;1H\x1b[33m Server only (not downloaded)\x1b[0m", row++);
    } else {
        printf("\x1b[%d;1H\x1b[90m No save data\x1b[0m", row++);
    }

    if (details->has_last_synced) {
        printf("\x1b[%d;1H Last synced: %.32s...", row++, details->last_synced_hash);
    }

    // Footer
    printf("\x1b[%d;1H\x1b[90m Press B to close\x1b[0m", TOP_ROWS);

    // Draw to both buffers to prevent flicker
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();

    // Wait for B button
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_B) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

#include "config.h"

// Draw config editor menu
static void draw_config_menu(const AppConfig *config, int selected) {
    consoleSelect(&top_screen);
    consoleClear();

    int row = 1;

    printf("\x1b[%d;1H\x1b[36m--- Configuration ---\x1b[0m", row++);
    row++;

    // Menu items
    const char *items[] = {
        "Server URL",
        "API Key",
        "Save & Exit",
        "Cancel"
    };
    const int item_count = 4;

    for (int i = 0; i < item_count; i++) {
        const char *cursor = (i == selected) ? ">" : " ";
        const char *color = (i == selected) ? "\x1b[33m" : "\x1b[0m";

        printf("\x1b[%d;1H%s%s %s\x1b[0m", row++, color, cursor, items[i]);

        // Show current value for editable items
        if (i == 0) {
            printf("\x1b[%d;1H   \x1b[90m%.44s\x1b[0m", row++, config->server_url);
        } else if (i == 1) {
            // Mask API key for privacy
            int len = strlen(config->api_key);
            if (len > 4) {
                printf("\x1b[%d;1H   \x1b[90m%.4s****\x1b[0m", row++, config->api_key);
            } else {
                printf("\x1b[%d;1H   \x1b[90m(not set)\x1b[0m", row++);
            }
        }
        row++;
    }

    row++;
    printf("\x1b[%d;1H\x1b[90mConsole ID: %s\x1b[0m", row++, config->console_id);

    // Footer
    printf("\x1b[%d;1H\x1b[90m A: Edit | D-Pad: Navigate\x1b[0m", TOP_ROWS);
}

bool ui_show_config_editor(AppConfig *config) {
    // Make a working copy
    AppConfig working;
    memcpy(&working, config, sizeof(AppConfig));

    int selected = 0;
    bool changed = false;
    bool running = true;

    while (running && aptMainLoop()) {
        // Draw menu to both buffers
        for (int buf = 0; buf < 2; buf++) {
            draw_config_menu(&working, selected);
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }

        // Wait for input
        while (aptMainLoop()) {
            hidScanInput();
            u32 kDown = hidKeysDown();

            if (kDown & KEY_UP) {
                selected = (selected - 1 + 4) % 4;
                break;
            }
            if (kDown & KEY_DOWN) {
                selected = (selected + 1) % 4;
                break;
            }
            if (kDown & KEY_B) {
                // Cancel without saving
                running = false;
                break;
            }
            if (kDown & KEY_A) {
                if (selected == 0) {
                    // Edit server URL
                    if (config_edit_field("http://192.168.1.100:8000", working.server_url, MAX_URL_LEN)) {
                        changed = true;
                    }
                    break;
                } else if (selected == 1) {
                    // Edit API key
                    if (config_edit_field("your-api-key", working.api_key, MAX_API_KEY_LEN)) {
                        changed = true;
                    }
                    break;
                } else if (selected == 2) {
                    // Save & Exit
                    if (changed) {
                        memcpy(config, &working, sizeof(AppConfig));
                        config_save(config);
                    }
                    running = false;
                    break;
                } else if (selected == 3) {
                    // Cancel
                    changed = false;
                    running = false;
                    break;
                }
            }

            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }
    }

    return changed;
}
