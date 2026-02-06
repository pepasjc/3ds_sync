#include "common.h"
#include "config.h"
#include "network.h"
#include "sync.h"
#include "title.h"
#include "ui.h"

static AppConfig config;
static TitleInfo titles[MAX_TITLES];
static int title_count = 0;
static int selected = 0;
static int scroll_offset = 0;
static char status[MAX_URL_LEN + 64];

#define LIST_VISIBLE 27 // TOP_ROWS(30) - header(2) - footer(1)

static void scan_titles(void) {
    ui_draw_message("Scanning titles...");
    title_count = titles_scan(titles, MAX_TITLES);
    selected = 0;
    scroll_offset = 0;

    // Fetch game names from server
    if (title_count > 0) {
        ui_draw_message("Fetching game names...");
        titles_fetch_names(&config, titles, title_count);
    }
}

// Clamp scroll so the selected item is always visible
static void update_scroll(void) {
    if (selected < scroll_offset)
        scroll_offset = selected;
    if (selected >= scroll_offset + LIST_VISIBLE)
        scroll_offset = selected - LIST_VISIBLE + 1;
}

// Progress callback - lightweight update without screen clear or buffer swap
static void sync_progress(const char *message) {
    ui_update_progress(message);
    // Don't call gfxSwapBuffers here - let the main loop handle it
    // This avoids GPU overload during rapid sync operations
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    // Initialize services
    gfxInitDefault();
    ui_init();
    amInit();
    fsInit();

    ui_draw_message("Loading config...");

    char config_error[512];
    if (!config_load(&config, config_error, sizeof(config_error))) {
        char msg[640];
        snprintf(msg, sizeof(msg),
            "\x1b[31mConfig error:\x1b[0m\n\n%s\n\n"
            "Expected file at:\n"
            "  %s\n\n"
            "With contents:\n"
            "  server_url=http://<pc-ip>:8000\n"
            "  api_key=<your-key>\n\n"
            "Press START to exit.",
            config_error, CONFIG_PATH);
        ui_draw_message(msg);

        while (aptMainLoop()) {
            gspWaitForVBlank();
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
        }

        fsExit();
        amExit();
        gfxExit();
        return 0;
    }

    // Initialize network
    if (!network_init()) {
        ui_draw_message(
            "\x1b[31mFailed to init network!\x1b[0m\n\n"
            "Make sure WiFi is enabled.\n\n"
            "Press START to exit.");

        while (aptMainLoop()) {
            gspWaitForVBlank();
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
        }

        fsExit();
        amExit();
        gfxExit();
        return 0;
    }

    // Initial title scan
    scan_titles();

    snprintf(status, sizeof(status), "Server: %.200s", config.server_url);
    ui_draw_title_list(titles, title_count, selected, scroll_offset);
    ui_draw_status(status);

    // Main loop
    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
        u32 kDown = hidKeysDown();

        bool redraw = false;

        if (kDown & KEY_START)
            break;

        if (kDown & KEY_DOWN && title_count > 0) {
            selected = (selected + 1) % title_count;
            update_scroll();
            redraw = true;
        }

        if (kDown & KEY_UP && title_count > 0) {
            selected = (selected - 1 + title_count) % title_count;
            update_scroll();
            redraw = true;
        }

        // Page down
        if (kDown & KEY_RIGHT && title_count > 0) {
            selected += LIST_VISIBLE;
            if (selected >= title_count) selected = title_count - 1;
            update_scroll();
            redraw = true;
        }

        // Page up
        if (kDown & KEY_LEFT && title_count > 0) {
            selected -= LIST_VISIBLE;
            if (selected < 0) selected = 0;
            update_scroll();
            redraw = true;
        }

        if (kDown & KEY_Y) {
            scan_titles();
            snprintf(status, sizeof(status), "Rescanned. %d title(s) found.", title_count);
            redraw = true;
        }

        if (kDown & KEY_A && title_count > 0) {
            SyncResult res = sync_title(&config, &titles[selected], sync_progress);
            if (res == SYNC_OK) {
                snprintf(status, sizeof(status), "Uploaded: %.40s", titles[selected].name);
                titles[selected].in_conflict = false;  // Resolved by uploading
            } else {
                snprintf(status, sizeof(status), "\x1b[31mUpload failed\x1b[0m: %s",
                    sync_result_str(res));
            }
            redraw = true;
        }

        if (kDown & KEY_B && title_count > 0) {
            // Force download from server
            SyncResult res = sync_download_title(&config, &titles[selected], sync_progress);
            if (res == SYNC_OK) {
                snprintf(status, sizeof(status), "Downloaded: %.40s", titles[selected].name);
                titles[selected].in_conflict = false;  // Resolved by downloading
            } else {
                snprintf(status, sizeof(status), "\x1b[31mDownload failed\x1b[0m: %s",
                    sync_result_str(res));
            }
            redraw = true;
        }

        if (kDown & KEY_X && title_count > 0) {
            // Clear all conflict flags before sync
            for (int i = 0; i < title_count; i++)
                titles[i].in_conflict = false;

            SyncSummary summary;
            bool ok = sync_all(&config, titles, title_count, sync_progress, &summary);
            if (ok) {
                // Mark conflicting titles in our list
                for (int i = 0; i < summary.conflicts && i < MAX_CONFLICT_DISPLAY; i++) {
                    for (int j = 0; j < title_count; j++) {
                        if (strcmp(titles[j].title_id_hex, summary.conflict_titles[i]) == 0) {
                            titles[j].in_conflict = true;
                            break;
                        }
                    }
                }

                if (summary.conflicts > 0) {
                    // Show conflict details - use game names
                    char conflict_msg[512];
                    int pos = snprintf(conflict_msg, sizeof(conflict_msg),
                        "\x1b[33mSync completed with %d conflict(s):\x1b[0m\n\n",
                        summary.conflicts);

                    // List conflicting titles by name
                    for (int i = 0; i < title_count && pos < (int)sizeof(conflict_msg) - 50; i++) {
                        if (titles[i].in_conflict) {
                            pos += snprintf(conflict_msg + pos, sizeof(conflict_msg) - pos,
                                "  %.35s\n", titles[i].name);
                        }
                    }
                    if (summary.conflicts > MAX_CONFLICT_DISPLAY) {
                        pos += snprintf(conflict_msg + pos, sizeof(conflict_msg) - pos,
                            "  ...and %d more\n", summary.conflicts - MAX_CONFLICT_DISPLAY);
                    }

                    snprintf(conflict_msg + pos, sizeof(conflict_msg) - pos,
                        "\nConflicts shown in \x1b[31mred\x1b[0m.\n"
                        "Select and use A/B to resolve.\n\n"
                        "Press any button to continue.");

                    ui_draw_message(conflict_msg);
                    // Wait for any button press
                    while (aptMainLoop()) {
                        gspWaitForVBlank();
                        hidScanInput();
                        if (hidKeysDown()) break;
                    }

                    snprintf(status, sizeof(status),
                        "Up:%d Dn:%d OK:%d \x1b[33mConflict:%d\x1b[0m Fail:%d",
                        summary.uploaded, summary.downloaded, summary.up_to_date,
                        summary.conflicts, summary.failed);
                } else {
                    snprintf(status, sizeof(status),
                        "Up:%d Dn:%d OK:%d Fail:%d",
                        summary.uploaded, summary.downloaded, summary.up_to_date,
                        summary.failed);
                }
            } else {
                snprintf(status, sizeof(status), "\x1b[31mSync failed!\x1b[0m Check server.");
            }
            redraw = true;
        }

        if (redraw) {
            ui_draw_title_list(titles, title_count, selected, scroll_offset);
            ui_draw_status(status);
        }
    }

    // Cleanup
    network_exit();
    fsExit();
    amExit();
    gfxExit();
    return 0;
}
