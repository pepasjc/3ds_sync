/*
 * ui.c — GX textured-font console via gxflux.
 *
 * gxflux registers itself as stdout, so positioned/coloured text is emitted
 * with printf + ANSI escape codes (gfx_con_set_pos, C_ and B_ colour macros).
 * gfx_con_draw() rasterises the current console grid through GX each frame.
 */

#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <gccore.h>
#include <gxflux/gfx.h>
#include <gxflux/gfx_con.h>

static int  g_rows = 25;
static int  g_cols = 40;
static char g_status[200];
static bool g_status_err = false;

#define HEADER_ROWS 2   /* title line + separator */
#define FOOTER_ROWS 2   /* hint line + status line */

static const char *color_escape(int color) {
    switch (color) {
        case UI_GREEN:  return C_IGREEN;
        case UI_RED:    return C_IRED;
        case UI_YELLOW: return C_IYELLOW;
        case UI_CYAN:   return C_ICYAN;
        case UI_BLUE:   return C_IBLUE;
        case UI_GREY:   return C_BLACK;    /* bold-off black reads as grey */
        case UI_WHITE:
        default:        return C_IWHITE;
    }
}

void ui_init(void) {
    /* gxflux reads the active video state (gfx_video_get_standard) and the
     * GS must already be alive — VIDEO_Init() has to run first or the screen
     * stays black. */
    VIDEO_Init();

    GXRModeObj rmode;
    gfx_video_standard_t standard = gfx_video_get_standard();
    gfx_video_get_modeobj(&rmode, standard, GFX_MODE_DEFAULT);
    gfx_video_init(&rmode);
    gfx_init();

    gfx_screen_coords_t coords;
    coords.x = 0.0f;
    coords.y = 0.0f;
    coords.w = (f32)gfx_video_get_width();
    coords.h = (f32)gfx_video_get_height();
    gfx_con_init(&coords);

    g_rows = gfx_con_get_rows();
    g_cols = gfx_con_get_columns();
    if (g_rows < 8)  g_rows = 25;
    if (g_cols < 16) g_cols = 40;
    if (g_cols > 80) g_cols = 80;

    gfx_con_clear();
}

int ui_rows(void) { return g_rows; }
int ui_cols(void) { return g_cols; }
int ui_list_top(void) { return HEADER_ROWS; }
int ui_list_visible(void) {
    int v = g_rows - HEADER_ROWS - FOOTER_ROWS;
    return v < 1 ? 1 : v;
}

void ui_clear(void) {
    gfx_con_clear();
}

void ui_flush(void) {
    /* gxflux contract: gfx_frame_start() returns false while the previous
     * frame is still in flight (WAITDRAWDONE/WAITVSYNC — it only becomes
     * READY at the retrace after GX draw-done).  Issuing GX commands anyway
     * wedges the real Flipper FIFO (hard freeze on hardware; Dolphin's HLE
     * GX masks it).  Wait a few vsyncs for READY; if it never comes, keep
     * the previous frame instead of corrupting the pipeline. */
    int tries = 8;
    while (!gfx_frame_start()) {
        if (--tries <= 0) return;
        VIDEO_WaitVSync();
    }
    gfx_con_draw();
    gfx_frame_end();
}

void ui_text(int row, int col, int color, const char *fmt, ...) {
    if (row < 0 || row >= g_rows) return;
    gfx_con_set_pos((u8)(row + 1), (u8)(col + 1));   /* escapes are 1-based */
    fputs(color_escape(color), stdout);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputs(CON_COLRESET, stdout);
}

void ui_text_hl(int row, bool selected, int color, const char *fmt, ...) {
    if (row < 0 || row >= g_rows) return;

    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    int width = g_cols;
    if (width > (int)sizeof(line) - 1) width = (int)sizeof(line) - 1;

    char padded[200];
    snprintf(padded, sizeof(padded), "%-*.*s", width, width, line);

    gfx_con_set_pos((u8)(row + 1), 1);
    if (selected) {
        fputs(B_BLUE, stdout);
        fputs(C_IWHITE, stdout);
    } else {
        fputs(color_escape(color), stdout);
    }
    fputs(padded, stdout);
    fputs(CON_COLRESET, stdout);
}

void ui_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    g_status_err = false;
}

void ui_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    g_status_err = true;
}

const char *ui_status_text(void) { return g_status; }
bool ui_status_is_error(void) { return g_status_err; }

const char *ui_view_name(AppView view) {
    switch (view) {
        case APP_VIEW_ROMS:      return "ROMs";
        case APP_VIEW_LOCAL:     return "Local";
        case APP_VIEW_DOWNLOADS: return "Downloads";
        case APP_VIEW_SAVES:     return "VMC";
        case APP_VIEW_CARDA:     return "Card A";
        case APP_VIEW_CARDB:     return "Card B";
        case APP_VIEW_SERVER:    return "Server";
        case APP_VIEW_CONFIG:    return "Config";
        default:                 return "?";
    }
}

void ui_draw_header(const SyncState *state, AppView view) {
    /* Row 0: app + view + page index.  Row 1: net / sd status. */
    ui_text(0, 0, UI_CYAN, "gcsync v%s", APP_VERSION);
    ui_text(0, 22, UI_WHITE, "[%d/%d] %s",
            (int)view + 1, (int)APP_VIEW_COUNT, ui_view_name(view));

    const char *net = state->net_ready
        ? (state->dhcp_ok ? state->ip : "link?")
        : "no-net";
    int net_color = (state->net_ready && state->dhcp_ok) ? UI_GREEN
                  : state->net_ready ? UI_YELLOW : UI_RED;
    ui_text(1, 0, net_color, "net:%s", net);
    ui_text(1, 22, state->sd_ready ? UI_GREEN : UI_RED,
            "sd:%s", state->sd_ready ? sd_device_to_str(state->sd_device) : "none");
}

void ui_draw_footer(const char *hint) {
    int hint_row   = g_rows - 2;
    int status_row = g_rows - 1;
    if (hint && hint[0]) ui_text(hint_row, 0, UI_GREY, "%s", hint);
    if (g_status[0]) {
        ui_text(status_row, 0, g_status_err ? UI_RED : UI_GREEN, "%s", g_status);
    }
}

void ui_draw_message(const char *title, const char *message) {
    ui_clear();
    ui_text(2, 2, UI_YELLOW, "%s", title);
    /* Print each line of the (possibly multi-line) message. */
    int row = 4;
    const char *p = message;
    char line[128];
    while (p && *p && row < g_rows - 1) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        ui_text(row++, 2, UI_WHITE, "%s", line);
        if (!nl) break;
        p = nl + 1;
    }
}
