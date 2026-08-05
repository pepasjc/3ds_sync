/*
 * ui.c — OSScreen text console, mirrored on TV + GamePad.
 *
 * OSScreen only draws white text, so the colour indices from ui.h are
 * rendered as one-character gutter marks in column 0 (and the selection is a
 * '>' caret) rather than real colours.  Everything else matches the GameCube
 * client's ui.h contract so the view bodies port unchanged.
 *
 * Framebuffers live in MEM1, which ProcUI reclaims whenever the app drops to
 * the background (HOME menu).  ui_acquire_foreground / ui_release_foreground
 * are registered as ProcUI callbacks so the buffers are torn down and rebuilt
 * around every foreground transition — without this the app hangs on HOME.
 */

#include "ui.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <coreinit/screen.h>
#include <proc_ui/procui.h>

/* The GamePad is the smaller of the two screens (854x480 vs 1280x720); size
 * the shared grid so the mirrored output fits there too. */
#define UI_ROWS 18
#define UI_COLS 64

#define HEADER_ROWS 2   /* title bar + status line */
#define FOOTER_ROWS 2   /* hint line + status line */

#define FRM_HEAP_TAG 0x33445353   /* "3DSS" */

static char    g_grid[UI_ROWS][UI_COLS + 1];
static char    g_mark[UI_ROWS];        /* gutter glyph per row */
static char    g_status[200];
static bool    g_status_err = false;

static void    *g_buf_tv = NULL, *g_buf_drc = NULL;
static uint32_t g_size_tv = 0, g_size_drc = 0;
static bool     g_foreground = false;
static volatile bool g_repaint_needed = false;

bool ui_consume_repaint_request(void) {
    if (!g_repaint_needed) return false;
    g_repaint_needed = false;
    return true;
}

int ui_rows(void) { return UI_ROWS; }
int ui_cols(void) { return UI_COLS; }
int ui_list_top(void) { return HEADER_ROWS; }
int ui_list_visible(void) {
    int v = UI_ROWS - HEADER_ROWS - FOOTER_ROWS;
    return v < 1 ? 1 : v;
}

/* One-character gutter mark standing in for colour. */
static char color_mark(int color) {
    switch (color) {
        case UI_GREEN:  return '+';
        case UI_RED:    return '!';
        case UI_YELLOW: return '*';
        case UI_CYAN:   return ':';
        case UI_BLUE:   return '#';
        case UI_GREY:   return '.';
        case UI_WHITE:
        default:        return ' ';
    }
}

/* ---- ProcUI foreground handling ---- */

uint32_t ui_acquire_foreground(void *ctx) {
    (void)ctx;
    if (g_foreground) return 0;

    MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
    if (!heap) return 0;
    MEMRecordStateForFrmHeap(heap, FRM_HEAP_TAG);

    OSScreenInit();
    g_size_tv  = OSScreenGetBufferSizeEx(SCREEN_TV);
    g_size_drc = OSScreenGetBufferSizeEx(SCREEN_DRC);

    /* OSScreenSetBufferEx wants 0x100-aligned memory. */
    g_buf_tv  = MEMAllocFromFrmHeapEx(heap, g_size_tv,  0x100);
    g_buf_drc = MEMAllocFromFrmHeapEx(heap, g_size_drc, 0x100);
    if (!g_buf_tv || !g_buf_drc) {
        MEMFreeByStateToFrmHeap(heap, FRM_HEAP_TAG);
        g_buf_tv = g_buf_drc = NULL;
        return 0;
    }

    OSScreenSetBufferEx(SCREEN_TV,  g_buf_tv);
    OSScreenSetBufferEx(SCREEN_DRC, g_buf_drc);
    OSScreenEnableEx(SCREEN_TV,  TRUE);
    OSScreenEnableEx(SCREEN_DRC, TRUE);

    /* Both buffers of each screen are freshly allocated MEM1 holding whatever
     * the previous owner left behind.  Clear and flip twice so neither the
     * visible nor the work buffer can show garbage before the first repaint. */
    for (int i = 0; i < 2; i++) {
        OSScreenClearBufferEx(SCREEN_TV,  0x00000000);
        OSScreenClearBufferEx(SCREEN_DRC, 0x00000000);
        DCFlushRange(g_buf_tv,  g_size_tv);
        DCFlushRange(g_buf_drc, g_size_drc);
        OSScreenFlipBuffersEx(SCREEN_TV);
        OSScreenFlipBuffersEx(SCREEN_DRC);
    }

    g_foreground = true;
    g_repaint_needed = true;
    return 0;
}

uint32_t ui_release_foreground(void *ctx) {
    (void)ctx;
    if (!g_foreground) return 0;

    OSScreenShutdown();
    MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
    if (heap) MEMFreeByStateToFrmHeap(heap, FRM_HEAP_TAG);
    g_buf_tv = g_buf_drc = NULL;
    g_foreground = false;
    return 0;
}

void ui_init(void) {
    memset(g_grid, 0, sizeof(g_grid));
    memset(g_mark, ' ', sizeof(g_mark));

    ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, ui_acquire_foreground, NULL, 100);
    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, ui_release_foreground, NULL, 100);

    /* WHBProcInit leaves us already in the foreground and does not replay the
     * ACQUIRE callback for that initial state — do it by hand. */
    ui_acquire_foreground(NULL);
    ui_clear();
}

void ui_shutdown(void) {
    ui_release_foreground(NULL);
}

/* ---- drawing ---- */

void ui_clear(void) {
    for (int r = 0; r < UI_ROWS; r++) {
        memset(g_grid[r], ' ', UI_COLS);
        g_grid[r][UI_COLS] = '\0';
        g_mark[r] = ' ';
    }
}

static void draw_screen(OSScreenID screen) {
    OSScreenClearBufferEx(screen, 0x00000000);
    char line[UI_COLS + 4];
    for (int r = 0; r < UI_ROWS; r++) {
        /* Trim trailing blanks so OSScreen does less work per row. */
        int len = UI_COLS;
        while (len > 0 && g_grid[r][len - 1] == ' ') len--;
        if (len == 0 && g_mark[r] == ' ') continue;
        line[0] = g_mark[r];
        line[1] = ' ';
        memcpy(line + 2, g_grid[r], (size_t)len);
        line[2 + len] = '\0';
        OSScreenPutFontEx(screen, 0, (uint32_t)r, line);
    }
}

void ui_flush(void) {
    if (!g_foreground) return;
    draw_screen(SCREEN_TV);
    draw_screen(SCREEN_DRC);
    DCFlushRange(g_buf_tv,  g_size_tv);
    DCFlushRange(g_buf_drc, g_size_drc);
    OSScreenFlipBuffersEx(SCREEN_TV);
    OSScreenFlipBuffersEx(SCREEN_DRC);
}

/* Copy ``src`` into row ``row`` starting at ``col``, clipped to the grid. */
static void put_at(int row, int col, const char *src) {
    if (row < 0 || row >= UI_ROWS || col >= UI_COLS) return;
    if (col < 0) col = 0;
    for (int i = 0; src[i] && col + i < UI_COLS; i++) {
        char c = src[i];
        if (c == '\t') c = ' ';
        if ((unsigned char)c < 0x20) c = ' ';
        g_grid[row][col + i] = c;
    }
}

void ui_text(int row, int col, int color, const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    put_at(row, col, line);
    if (row >= 0 && row < UI_ROWS && g_mark[row] == ' ')
        g_mark[row] = color_mark(color);
}

void ui_text_hl(int row, bool selected, int color, const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    put_at(row, 0, line);
    if (row >= 0 && row < UI_ROWS)
        g_mark[row] = selected ? '>' : color_mark(color);
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
        case APP_VIEW_ROMS:      return "GAME CATALOG";
        case APP_VIEW_LOCAL:     return "LOCAL GAMES";
        case APP_VIEW_DOWNLOADS: return "DOWNLOADS";
        case APP_VIEW_GCCARDS:   return "GC MEMCARDS (NINTENDONT)";
        case APP_VIEW_SERVER:    return "SERVER GC SAVES";
        case APP_VIEW_VWII:      return "VWII SAVES";
        case APP_VIEW_WIIU:      return "WII U SAVES";
        case APP_VIEW_CONFIG:    return "CONFIG";
        default:                 return "?";
    }
}

void ui_draw_header(const SyncState *state, AppView view) {
    char bar[UI_COLS + 1];
    char right[16];
    snprintf(right, sizeof(right), "%d/%d", (int)view + 1, (int)APP_VIEW_COUNT);

    memset(bar, ' ', UI_COLS);
    bar[UI_COLS] = '\0';
    const char *title = ui_view_name(view);
    int tl = (int)strlen(title);
    if (tl > UI_COLS - 6) tl = UI_COLS - 6;
    memcpy(bar, title, (size_t)tl);
    int rl = (int)strlen(right);
    memcpy(bar + UI_COLS - rl, right, (size_t)rl);
    put_at(0, 0, bar);
    g_mark[0] = '#';

    const char *net = state->net_ready ? state->ip : "no-net";
    ui_text(1, 0, state->net_ready ? UI_GREEN : UI_RED, "net:%s", net);
    ui_text(1, 24, state->sd_ready ? UI_GREEN : UI_RED,
            "sd:%s", state->sd_ready ? "ok" : "none");
    ui_text(1, 34, state->mocha_ok ? UI_GREEN : UI_YELLOW,
            "mocha:%s", state->mocha_ok ? "ok" : "off");
    ui_text(1, 50, UI_CYAN, "v%s", APP_VERSION);
}

void ui_draw_footer(const char *hint) {
    int hint_row   = UI_ROWS - 2;
    int status_row = UI_ROWS - 1;
    if (hint && hint[0]) ui_text(hint_row, 0, UI_GREY, "%s", hint);
    if (g_status[0])
        ui_text(status_row, 0, g_status_err ? UI_RED : UI_GREEN, "%s", g_status);
}

void ui_draw_message(const char *title, const char *message) {
    ui_clear();
    ui_text(1, 0, UI_YELLOW, "%s", title);

    int row = 3;
    const char *p = message;
    char line[UI_COLS + 1];
    while (p && *p && row < UI_ROWS - 1) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        ui_text(row++, 0, UI_WHITE, "%s", line);
        if (!nl) break;
        p = nl + 1;
    }
}
