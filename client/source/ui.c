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
    consoleClear();

    // Header
    printf("\x1b[1;1H\x1b[36m--- 3DS Save Sync v%s ---\x1b[0m\n", APP_VERSION);
    printf("\x1b[2;1H");

    if (count == 0) {
        printf("\n  No titles with save data found.\n");
        printf("  Make sure you have games installed.\n");
        return;
    }

    // Title list (scrollable)
    int visible = LIST_ROWS;
    if (visible > count) visible = count;

    for (int i = 0; i < visible; i++) {
        int idx = scroll_offset + i;
        if (idx >= count) break;

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
            color = "";
        }

        // Truncate name to fit (50 cols - cursor(2) - media(5) - space(1) = 42 chars)
        char display_name[43];
        snprintf(display_name, sizeof(display_name), "%.42s", t->name);

        printf(" %s %s%-4s %s\x1b[0m\n",
            cursor,
            color,
            media_type_str(t->media_type),
            display_name);
    }

    // Footer with count
    printf("\x1b[%d;1H\x1b[90m %d title(s) | D-Pad: navigate\x1b[0m", TOP_ROWS, count);
}

void ui_draw_status(const char *status_line) {
    consoleSelect(&bottom_screen);
    consoleClear();

    printf("\x1b[1;1H\x1b[36mActions:\x1b[0m\n\n");
    printf(" A  - Upload save to server\n");
    printf(" B  - Download save from server\n");
    printf(" X  - Sync all (SD only)\n");
    printf(" Y  - Rescan titles\n");
    printf(" SELECT - Check for updates\n");
    printf(" START - Exit\n");
    printf("\n\x1b[36mCyan\x1b[0m = cartridge (A/B only)\n");
    printf("\n\x1b[90m%s\x1b[0m", status_line ? status_line : "Ready.");
}

void ui_draw_message(const char *msg) {
    consoleSelect(&bottom_screen);
    consoleClear();
    printf("\x1b[1;1H%s\n", msg);
}

void ui_update_progress(const char *msg) {
    // Lightweight update: just overwrite line 1 without clearing whole screen
    consoleSelect(&bottom_screen);
    printf("\x1b[1;1H\x1b[2K%s", msg);  // Move to line 1, clear line, print
}

void ui_clear(void) {
    consoleSelect(&top_screen);
    consoleClear();
    consoleSelect(&bottom_screen);
    consoleClear();
}
