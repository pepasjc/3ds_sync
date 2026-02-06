#include "ui.h"

static PrintConsole top_screen;
static PrintConsole bottom_screen;

#define TOP_ROWS    30
#define TOP_COLS    50
#define LIST_ROWS   (TOP_ROWS - 3) // Reserve top 2 for header, bottom 1 for count

void ui_init(void) {
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
    printf("\x1b[2;1H%-*s", BOT_COLS, "");
    printf("\x1b[3;1H A  - Upload save to server%-*s", BOT_COLS - 27, "");
    printf("\x1b[4;1H B  - Download save from server%-*s", BOT_COLS - 31, "");
    printf("\x1b[5;1H X  - Sync all (SD only)%-*s", BOT_COLS - 24, "");
    printf("\x1b[6;1H Y  - Rescan titles%-*s", BOT_COLS - 19, "");
    printf("\x1b[7;1H SELECT - Check for updates%-*s", BOT_COLS - 27, "");
    printf("\x1b[8;1H START - Exit%-*s", BOT_COLS - 13, "");
    printf("\x1b[9;1H%-*s", BOT_COLS, "");
    printf("\x1b[10;1H\x1b[36mCyan\x1b[0m = cartridge (A/B only)%-*s", BOT_COLS - 27, "");
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
