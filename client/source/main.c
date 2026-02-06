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
            if (res == SYNC_OK)
                snprintf(status, sizeof(status), "Uploaded: %s", titles[selected].title_id_hex);
            else
                snprintf(status, sizeof(status), "\x1b[31mSync failed\x1b[0m: %s (err %d)",
                    titles[selected].title_id_hex, res);
            redraw = true;
        }

        if (kDown & KEY_X && title_count > 0) {
            int synced = sync_all(&config, titles, title_count, sync_progress);
            if (synced >= 0)
                snprintf(status, sizeof(status), "Sync complete: %d title(s) synced.", synced);
            else
                snprintf(status, sizeof(status), "\x1b[31mSync failed!\x1b[0m Check server.");
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
