/*
 * gcsync — GameCube Save Sync client.
 *
 * Phase 1: boot + UI + controller-editable config.
 * Phase 2: ROM catalog browse + SD install (sd:/games) + resumable downloads.
 *
 * Boot: ui_init -> mount SD -> config -> BBA net -> CARD init -> menu loop.
 * Memory-card / VMC save sync (Phase 3) and GameID (Phase 4) are still stubbed.
 */

#include "common.h"
#include "config.h"
#include "gcnet.h"
#include "ui.h"
#include "roms.h"
#include "downloads.h"
#include "saves.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/card.h>
#include <ogc/pad.h>
#include <fat.h>
#include <sdcard/gcsd.h>

static SyncState     g_state;
static AppView       g_view = APP_VIEW_ROMS;
static RomCatalog    g_catalog;
static LocalRomList  g_local;
static DownloadList  g_downloads;

static GcSaveList     g_carda, g_cardb;     /* slot A (chan 0), slot B (chan 1) */
static ServerSaveList g_server;
static SaveVmcList    g_vmc;

static int g_cfg_sel    = 0;
static int g_rom_sel    = 0, g_rom_scroll   = 0;
static int g_local_sel  = 0, g_local_scroll = 0;
static int g_dl_sel     = 0, g_dl_scroll    = 0;
static int g_ca_sel     = 0, g_ca_scroll    = 0;
static int g_cb_sel     = 0, g_cb_scroll    = 0;
static int g_sv_sel     = 0, g_sv_scroll    = 0;
static int g_vmc_sel    = 0, g_vmc_scroll   = 0;

static char g_scratch[256 * 1024];   /* catalog JSON page buffer */

/* Live download progress (updated from progress_cb). */
static volatile uint64_t g_active_done  = 0;
static volatile uint64_t g_active_total = 0;
static volatile bool     g_pause_req    = false;
static uint64_t          g_last_draw    = 0;

static void redraw(void);   /* forward decl: long ops flush mid-run */

/* ---- helpers ---- */

static void human_size(uint64_t b, char *o, size_t n) {
    if (b >= (1ULL << 30)) snprintf(o, n, "%lluMB", (unsigned long long)(b >> 20));
    else if (b >= (1ULL << 20)) snprintf(o, n, "%lluMB", (unsigned long long)(b >> 20));
    else if (b >= (1ULL << 10)) snprintf(o, n, "%lluKB", (unsigned long long)(b >> 10));
    else snprintf(o, n, "%lluB", (unsigned long long)b);
}

static void clamp_scroll(int *sel, int *scroll, int count) {
    if (count == 0) { *sel = 0; *scroll = 0; return; }
    if (*sel < 0) *sel = 0;
    if (*sel >= count) *sel = count - 1;
    int vis = ui_list_visible();
    if (vis < 1) vis = 1;
    if (*sel < *scroll) *scroll = *sel;
    if (*sel >= *scroll + vis) *scroll = *sel - vis + 1;
    if (*scroll < 0) *scroll = 0;
}

/* ---- SD mounting ---- */

static const DISC_INTERFACE *sd_iface(SdDevice dev) {
    switch (dev) {
        case SD_DEV_GECKO_A: return &__io_gcsda;
        case SD_DEV_GECKO_B: return &__io_gcsdb;
        case SD_DEV_SP2:
        default:             return &__io_gcsd2;
    }
}

static bool mount_sd(SyncState *st) {
    static const SdDevice order[] = { SD_DEV_SP2, SD_DEV_GECKO_A, SD_DEV_GECKO_B };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (fatMountSimple(SD_MOUNT_NAME, sd_iface(order[i]))) {
            st->sd_ready  = true;
            st->sd_device = order[i];
            strncpy(st->sd_root, SD_ROOT, sizeof(st->sd_root) - 1);
            st->sd_root[sizeof(st->sd_root) - 1] = '\0';
            return true;
        }
    }
    st->sd_ready = false;
    return false;
}

static void show_boot(const char *msg) {
    ui_clear();
    ui_draw_message("gcsync boot", msg);
    ui_flush();
}

/* ---- Controller text editor (on-screen character picker) ---- */

static const char CHARSET[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.:/_-?=&%@";

static char charset_step(char c, int dir) {
    int n = (int)sizeof(CHARSET) - 1;
    const char *pos = strchr(CHARSET, c);
    int idx = pos ? (int)(pos - CHARSET) : 1;
    idx = (idx + dir + n) % n;
    return CHARSET[idx];
}

static void draw_edit(const char *label, const char *buf, int cur) {
    ui_clear();
    ui_draw_header(&g_state, APP_VIEW_CONFIG);
    ui_text(ui_list_top(), 2, UI_YELLOW, "Edit %s", label);
    ui_text(ui_list_top() + 2, 2, UI_WHITE, "%s", buf[0] ? buf : "(empty)");

    char caret[200];
    int cols = ui_cols();
    if (cur > cols - 4) cur = cols - 4;
    memset(caret, ' ', sizeof(caret));
    if (cur >= 0 && cur < (int)sizeof(caret) - 1) caret[cur] = '^';
    caret[cols < (int)sizeof(caret) ? cols : (int)sizeof(caret) - 1] = '\0';
    ui_text(ui_list_top() + 3, 2, UI_CYAN, "%s", caret);

    ui_draw_footer("Up/Dn=char  L/R=move  Z=insert  X=delete  A=ok  B=cancel");
    ui_flush();
}

static bool edit_text(char *out, size_t cap, const char *label) {
    char buf[256];
    strncpy(buf, out, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    int len = (int)strlen(buf);
    int cur = len;
    int maxlen = (int)(cap < sizeof(buf) ? cap : sizeof(buf)) - 1;

    draw_edit(label, buf, cur);
    for (;;) {
        VIDEO_WaitVSync();
        PAD_ScanPads();
        u32 d = PAD_ButtonsDown(0);
        if (d == 0) continue;   /* redraw only on input */

        if (d & (PAD_BUTTON_A | PAD_BUTTON_START)) {
            strncpy(out, buf, cap - 1); out[cap - 1] = '\0'; return true;
        }
        if (d & PAD_BUTTON_B) return false;
        if ((d & PAD_BUTTON_LEFT)  && cur > 0)   cur--;
        if ((d & PAD_BUTTON_RIGHT) && cur < len) cur++;
        if (d & (PAD_BUTTON_UP | PAD_BUTTON_DOWN)) {
            int dir = (d & PAD_BUTTON_UP) ? 1 : -1;
            if (cur == len) { if (len < maxlen) { buf[len++] = charset_step(' ', dir); buf[len] = '\0'; } }
            else buf[cur] = charset_step(buf[cur], dir);
        }
        if (d & PAD_BUTTON_X) {
            if (cur < len) { memmove(&buf[cur], &buf[cur + 1], len - cur); len--; }
            else if (len > 0) { buf[--len] = '\0'; cur = len; }
        }
        if (d & PAD_TRIGGER_Z) {
            if (len < maxlen) { memmove(&buf[cur + 1], &buf[cur], len - cur + 1); buf[cur] = ' '; len++; }
        }
        draw_edit(label, buf, cur);
    }
}

/* ---- Config view ---- */

typedef enum {
    CF_SERVER = 0, CF_APIKEY, CF_NETMODE, CF_IP, CF_NM, CF_GW,
    CF_SDDEV, CF_GAMES, CF_GIDA, CF_GIDB, CF_SAVE, CF_COUNT
} CfgField;

static void draw_config(void) {
    int top = ui_list_top();
    const char *net = g_state.use_static_ip ? "static" : "DHCP";
    ui_text_hl(top + CF_SERVER,  g_cfg_sel == CF_SERVER,  UI_WHITE, " Server URL : %s", g_state.server_url);
    ui_text_hl(top + CF_APIKEY,  g_cfg_sel == CF_APIKEY,  UI_WHITE, " API Key    : %s", g_state.api_key);
    ui_text_hl(top + CF_NETMODE, g_cfg_sel == CF_NETMODE, UI_WHITE, " Network    : %s", net);
    ui_text_hl(top + CF_IP,      g_cfg_sel == CF_IP,      UI_WHITE, " Static IP  : %s", g_state.static_ip);
    ui_text_hl(top + CF_NM,      g_cfg_sel == CF_NM,      UI_WHITE, " Netmask    : %s", g_state.static_netmask);
    ui_text_hl(top + CF_GW,      g_cfg_sel == CF_GW,      UI_WHITE, " Gateway    : %s", g_state.static_gateway);
    ui_text_hl(top + CF_SDDEV,   g_cfg_sel == CF_SDDEV,   UI_WHITE, " SD device  : %s", sd_device_to_str(g_state.sd_device));
    ui_text_hl(top + CF_GAMES,   g_cfg_sel == CF_GAMES,   UI_WHITE, " Games dir  : %s", g_state.games_folder);
    ui_text_hl(top + CF_GIDA,    g_cfg_sel == CF_GIDA,    UI_WHITE, " GameID A   : %s", g_state.mmce_mode[0] ? "on" : "off");
    ui_text_hl(top + CF_GIDB,    g_cfg_sel == CF_GIDB,    UI_WHITE, " GameID B   : %s", g_state.mmce_mode[1] ? "on" : "off");
    ui_text_hl(top + CF_SAVE,    g_cfg_sel == CF_SAVE,    UI_GREEN, " [ Save config to SD ]");
}

static void config_change(int dir) {
    switch (g_cfg_sel) {
        case CF_NETMODE: g_state.use_static_ip = !g_state.use_static_ip; break;
        case CF_SDDEV:
            g_state.sd_device = (SdDevice)(((int)g_state.sd_device + dir + SD_DEV_COUNT) % SD_DEV_COUNT);
            break;
        case CF_GIDA: g_state.mmce_mode[0] = !g_state.mmce_mode[0]; break;
        case CF_GIDB: g_state.mmce_mode[1] = !g_state.mmce_mode[1]; break;
        default: break;
    }
}

static void config_activate(void) {
    switch (g_cfg_sel) {
        case CF_SERVER: edit_text(g_state.server_url, sizeof(g_state.server_url), "Server URL"); break;
        case CF_APIKEY: edit_text(g_state.api_key, sizeof(g_state.api_key), "API Key"); break;
        case CF_IP:     edit_text(g_state.static_ip, sizeof(g_state.static_ip), "Static IP"); break;
        case CF_NM:     edit_text(g_state.static_netmask, sizeof(g_state.static_netmask), "Netmask"); break;
        case CF_GW:     edit_text(g_state.static_gateway, sizeof(g_state.static_gateway), "Gateway"); break;
        case CF_GAMES:  edit_text(g_state.games_folder, sizeof(g_state.games_folder), "Games folder");
                        roms_set_target(g_state.sd_root, g_state.games_folder); break;
        case CF_NETMODE: case CF_SDDEV: case CF_GIDA: case CF_GIDB: config_change(+1); break;
        case CF_SAVE:
            if (!g_state.sd_ready) ui_error("No SD mounted - cannot save config");
            else if (config_save(&g_state)) ui_status("Config saved to SD");
            else ui_error("Failed to write config");
            break;
        default: break;
    }
}

static void config_input(u32 d) {
    if (d & PAD_BUTTON_UP)    g_cfg_sel = (g_cfg_sel - 1 + CF_COUNT) % CF_COUNT;
    if (d & PAD_BUTTON_DOWN)  g_cfg_sel = (g_cfg_sel + 1) % CF_COUNT;
    if (d & PAD_BUTTON_LEFT)  config_change(-1);
    if (d & PAD_BUTTON_RIGHT) config_change(+1);
    if (d & PAD_BUTTON_A)     config_activate();
}

/* ---- Catalog / local / downloads ---- */

static void fetch_catalog(void) {
    if (!network_is_ready(&g_state)) { ui_error("Network not ready (%s)", g_state.ip); return; }
    ui_status("Fetching GC catalog...");
    redraw();
    bool ok = roms_fetch_catalog(&g_state, "GC", g_scratch, sizeof(g_scratch), &g_catalog);
    if (!ok) ui_error("%s", g_catalog.last_error);
    else     ui_status("Catalog: %d GC ROM(s)", g_catalog.count);
    g_rom_sel = 0; g_rom_scroll = 0;
    redraw();
}

static void scan_local(void) {
    if (!g_state.sd_ready) { ui_error("Storage not ready"); return; }
    ui_status("Scanning sd:/games...");
    redraw();
    roms_scan_local(&g_local);
    if (g_local.count > 0) ui_status("Local: %d ISO(s)", g_local.count);
    else ui_status("%s", g_local.last_error);
    redraw();
}

static int progress_cb(uint64_t done, uint64_t total) {
    g_active_done = done;
    g_active_total = total;
    PAD_ScanPads();
    if (PAD_ButtonsDown(0) & PAD_BUTTON_B) g_pause_req = true;
    if (done - g_last_draw >= (1U << 19) || (total && done >= total)) {
        g_last_draw = done;
        redraw();
    }
    return g_pause_req ? 1 : 0;
}

static void run_active_download(DownloadEntry *e) {
    if (!e) return;
    if (!g_state.sd_ready) { ui_error("No SD - cannot download"); return; }

    /* Ensure target dir exists. */
    char dir[256];
    strncpy(dir, e->target_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; roms_mkdir_p(dir); }

    g_active_done  = e->offset;
    g_active_total = e->total;
    g_pause_req    = false;
    g_last_draw    = e->offset;
    network_set_progress64_cb(progress_cb);

    e->status = DL_STATUS_ACTIVE;
    downloads_save(&g_downloads);

    uint64_t total = 0;
    int rc = network_download_rom_resumable(&g_state, e->rom_id, e->extract_format,
                                            e->target_path, e->offset, &total);
    network_set_progress64_cb(NULL);
    if (total > 0) e->total = total;

    if (rc == 0) {
        e->status = DL_STATUS_COMPLETED;
        e->offset = e->total > 0 ? e->total : g_active_done;
        ui_status("Done: %s", e->name);
    } else if (rc == 1) {
        e->status = DL_STATUS_PAUSED;
        e->offset = g_active_done;
        ui_status("Paused: %s (resume with A)", e->name);
    } else {
        e->status = DL_STATUS_ERROR;
        e->offset = g_active_done;
        ui_error("Download failed rc=%d: %s", rc, e->name);
    }
    downloads_save(&g_downloads);
    g_active_total = 0;
    redraw();
}

static void queue_selected_rom(bool run_now) {
    if (!g_state.sd_ready) { ui_error("No SD - cannot install"); return; }
    if (g_catalog.count == 0 || g_rom_sel >= g_catalog.count) return;
    DownloadEntry *e = downloads_upsert_from_catalog(&g_downloads, &g_catalog.items[g_rom_sel]);
    if (!e) { ui_error("Download list full"); return; }
    downloads_save(&g_downloads);
    if (run_now) run_active_download(e);
    else ui_status("Queued: %s", e->name);
}

/* ---- View rendering ---- */

static void draw_roms(void) {
    int top = ui_list_top(), vis = ui_list_visible();
    if (g_catalog.count == 0) {
        ui_text(top + 1, 2, UI_GREY, "%s",
                g_catalog.last_error[0] ? g_catalog.last_error : "No GC ROMs. Press A to fetch.");
        return;
    }
    int namew = ui_cols() - 12;
    if (namew < 8) namew = 8;
    for (int i = 0; i < vis; i++) {
        int idx = g_rom_scroll + i;
        if (idx >= g_catalog.count) break;
        RomEntry *r = &g_catalog.items[idx];
        char sz[16]; human_size(r->size, sz, sizeof(sz));
        bool queued = downloads_find(&g_downloads, r->rom_id) != NULL;
        char line[200];
        snprintf(line, sizeof(line), " %-*.*s %7s%s", namew, namew,
                 r->name, sz, queued ? " *" : "");
        ui_text_hl(top + i, idx == g_rom_sel, queued ? UI_GREEN : UI_WHITE, "%s", line);
    }
}

static void draw_local(void) {
    int top = ui_list_top(), vis = ui_list_visible();
    if (g_local.count == 0) {
        ui_text(top + 1, 2, UI_GREY, "%s",
                g_local.last_error[0] ? g_local.last_error : "No installed ISOs.");
        return;
    }
    int namew = ui_cols() - 12;
    if (namew < 8) namew = 8;
    for (int i = 0; i < vis; i++) {
        int idx = g_local_scroll + i;
        if (idx >= g_local.count) break;
        LocalRom *r = &g_local.items[idx];
        char sz[16]; human_size(r->size, sz, sizeof(sz));
        char line[200];
        snprintf(line, sizeof(line), " %-*.*s %7s", namew, namew, r->name, sz);
        ui_text_hl(top + i, idx == g_local_sel, UI_WHITE, "%s", line);
    }
}

static void draw_downloads(void) {
    int top = ui_list_top(), vis = ui_list_visible();
    if (g_downloads.count == 0) {
        ui_text(top + 1, 2, UI_GREY, "Queue empty. Queue ROMs from the ROMs view (X).");
        return;
    }
    for (int i = 0; i < vis; i++) {
        int idx = g_dl_scroll + i;
        if (idx >= g_downloads.count) break;
        DownloadEntry *e = &g_downloads.items[idx];
        uint64_t done = (e->status == DL_STATUS_ACTIVE) ? g_active_done : e->offset;
        uint64_t tot  = (e->status == DL_STATUS_ACTIVE && g_active_total) ? g_active_total : e->total;
        int pct = tot ? (int)(done * 100 / tot) : 0;
        int color = e->status == DL_STATUS_COMPLETED ? UI_GREEN
                  : e->status == DL_STATUS_ERROR     ? UI_RED
                  : e->status == DL_STATUS_ACTIVE    ? UI_YELLOW : UI_WHITE;
        int namew = ui_cols() - 22;
        if (namew < 6) namew = 6;
        char line[200];
        snprintf(line, sizeof(line), " %-9s %3d%% %-*.*s",
                 downloads_status_to_str(e->status), pct, namew, namew, e->name);
        ui_text_hl(top + i, idx == g_dl_sel, color, "%s", line);
    }
}

/* ---- Memory-card / server save sync ---- */

/* Modal yes/no.  A = yes, B = no. */
static bool confirm(const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char body[280];
    snprintf(body, sizeof(body), "%s\n\nA = Yes      B = No", msg);
    ui_clear();
    ui_draw_message("Confirm", body);
    ui_draw_footer("A = Yes   B = No");
    ui_flush();
    for (;;) {
        VIDEO_WaitVSync();
        PAD_ScanPads();
        u32 d = PAD_ButtonsDown(0);
        if (d & PAD_BUTTON_A) return true;
        if (d & PAD_BUTTON_B) return false;
    }
}

static void fill_card_names(GcSaveList *list) {
    for (int i = 0; i < list->count; i++) {
        list->items[i].name[0] = '\0';
        for (int j = 0; j < g_server.count; j++) {
            if (strcmp(g_server.items[j].title_id, list->items[i].title_id) == 0) {
                strncpy(list->items[i].name, g_server.items[j].name,
                        sizeof(list->items[i].name) - 1);
                break;
            }
        }
    }
}

static void mark_server_local(void) {
    for (int i = 0; i < g_server.count; i++) {
        ServerSave *s = &g_server.items[i];
        s->local = false;
        for (int j = 0; j < g_carda.count && !s->local; j++)
            if (strcmp(g_carda.items[j].title_id, s->title_id) == 0) s->local = true;
        for (int j = 0; j < g_cardb.count && !s->local; j++)
            if (strcmp(g_cardb.items[j].title_id, s->title_id) == 0) s->local = true;
    }
}

static void scan_card_view(int port, GcSaveList *list) {
    ui_status("Scanning memory card slot %c...", 'A' + port);
    redraw();
    saves_scan_card(port, list);
    fill_card_names(list);
    if (list->count > 0) ui_status("Slot %c: %d save(s)", 'A' + port, list->count);
    else ui_status("%s", list->last_error);
    redraw();
}

static void fetch_server(void) {
    if (!network_is_ready(&g_state)) { ui_error("Network not ready"); return; }
    ui_status("Fetching server saves...");
    redraw();
    saves_fetch_server(&g_state, g_scratch, sizeof(g_scratch), &g_server);
    mark_server_local();
    fill_card_names(&g_carda);
    fill_card_names(&g_cardb);
    if (g_server.count > 0) ui_status("Server: %d GC save(s)", g_server.count);
    else ui_error("%s", g_server.last_error);
    redraw();
}

static void upload_card_at(GcSaveList *list, int sel) {
    if (!network_is_ready(&g_state)) { ui_error("Network not ready"); return; }
    if (list->count == 0 || sel >= list->count) return;
    const GcSave *s = &list->items[sel];
    if (!confirm("Upload %s\nfrom slot %c to the server?", s->title_id, 'A' + list->port)) return;
    ui_status("Uploading %s...", s->title_id);
    redraw();
    char msg[128];
    int rc = saves_upload_card_game(&g_state, list->port, s, msg, sizeof(msg));
    if (rc < 0) ui_error("%s", msg); else ui_status("%s", msg);
    redraw();
}

static void restore_card_at(GcSaveList *list, int sel) {
    if (!network_is_ready(&g_state)) { ui_error("Network not ready"); return; }
    if (list->count == 0 || sel >= list->count) return;
    const char *tid = list->items[sel].title_id;
    int port = list->port;
    if (!confirm("Restore %s to slot %c?\nOverwrites the on-card save.", tid, 'A' + port)) return;
    ui_status("Restoring %s...", tid);
    redraw();
    char msg[128];
    int rc = saves_restore_card_game(&g_state, port, tid, msg, sizeof(msg));
    saves_scan_card(port, list);
    fill_card_names(list);
    if (rc < 0) ui_error("%s", msg); else ui_status("%s", msg);
    redraw();
}

/* Server view: restore the selected server save onto a memory-card slot. */
static void server_restore_to(int port) {
    if (!network_is_ready(&g_state)) { ui_error("Network not ready"); return; }
    if (g_server.count == 0 || g_sv_sel >= g_server.count) return;
    const char *tid = g_server.items[g_sv_sel].title_id;
    if (!confirm("Restore %s\nto memory card slot %c?\nOverwrites the on-card save.", tid, 'A' + port))
        return;
    ui_status("Restoring %s to slot %c...", tid, 'A' + port);
    redraw();
    char msg[128];
    int rc = saves_restore_card_game(&g_state, port, tid, msg, sizeof(msg));
    GcSaveList *list = port == 0 ? &g_carda : &g_cardb;
    saves_scan_card(port, list);
    fill_card_names(list);
    mark_server_local();
    if (rc < 0) ui_error("%s", msg); else ui_status("%s", msg);
    redraw();
}

static void draw_card(const GcSaveList *list, int sel, int scroll) {
    int top = ui_list_top(), vis = ui_list_visible();
    if (list->count == 0) {
        ui_text(top + 1, 2, UI_GREY, "%s",
                list->last_error[0] ? list->last_error : "No saves on this card.");
        return;
    }
    int namew = ui_cols() - 18;
    if (namew < 8) namew = 8;
    for (int i = 0; i < vis; i++) {
        int idx = scroll + i;
        if (idx >= list->count) break;
        const GcSave *s = &list->items[idx];
        const char *nm = s->name[0] ? s->name : s->title_id;
        char line[200];
        snprintf(line, sizeof(line), " %-*.*s %4d blk", namew, namew, nm, s->blocks);
        ui_text_hl(top + i, idx == sel, UI_WHITE, "%s", line);
    }
}

static void draw_server(int sel, int scroll) {
    int top = ui_list_top(), vis = ui_list_visible();
    if (g_server.count == 0) {
        ui_text(top + 1, 2, UI_GREY, "%s",
                g_server.last_error[0] ? g_server.last_error : "No GC saves on the server.");
        return;
    }
    int namew = ui_cols() - 6;
    if (namew < 8) namew = 8;
    for (int i = 0; i < vis; i++) {
        int idx = scroll + i;
        if (idx >= g_server.count) break;
        const ServerSave *s = &g_server.items[idx];
        const char *nm = s->name[0] ? s->name : s->title_id;
        char line[200];
        snprintf(line, sizeof(line), " %-*.*s %s", namew, namew, nm, s->local ? "L" : " ");
        ui_text_hl(top + i, idx == sel, s->local ? UI_GREEN : UI_WHITE, "%s", line);
    }
}

/* ---- VMC (full card images on SD) ---- */

static void scan_vmc_view(void) {
    if (!g_state.sd_ready) { ui_error("Storage not ready"); return; }
    ui_status("Scanning card images...");
    redraw();
    saves_scan_vmc(&g_vmc);
    if (g_vmc.count > 0) ui_status("VMC: %d card image(s)", g_vmc.count);
    else ui_status("%s", g_vmc.last_error);
    redraw();
}

static void upload_vmc_at(int sel) {
    if (!network_is_ready(&g_state)) { ui_error("Network not ready"); return; }
    if (g_vmc.count == 0 || sel >= g_vmc.count) return;
    const SaveVmc *v = &g_vmc.items[sel];
    if (!confirm("Import card image\n%s\nto server (split per game)?", v->filename)) return;
    ui_status("Importing %s...", v->filename);
    redraw();
    char msg[128];
    int rc = saves_upload_vmc(&g_state, v, msg, sizeof(msg));
    if (rc < 0) ui_error("%s", msg);
    else { ui_status("%s", msg); fetch_server(); }
    redraw();
}

static void pull_all_vmc(void) {
    if (!network_is_ready(&g_state)) { ui_error("Network not ready"); return; }
    if (g_server.count == 0) fetch_server();
    if (!confirm("Download ALL %d server GC saves\nas .gci into sd:/VMC/ ?", g_server.count)) return;
    ui_status("Pulling GCIs...");
    redraw();
    char msg[128];
    int rc = saves_pull_all(&g_state, &g_server, msg, sizeof(msg));
    if (rc < 0) ui_error("%s", msg); else ui_status("%s", msg);
    scan_vmc_view();
}

static void draw_vmc(void) {
    int top = ui_list_top(), vis = ui_list_visible();
    if (g_vmc.count == 0) {
        ui_text(top + 1, 2, UI_GREY, "%s",
                g_vmc.last_error[0] ? g_vmc.last_error : "No card images. Put .raw cards in sd:/VMC.");
        return;
    }
    int namew = ui_cols() - 10;
    if (namew < 8) namew = 8;
    for (int i = 0; i < vis; i++) {
        int idx = g_vmc_scroll + i;
        if (idx >= g_vmc.count) break;
        const SaveVmc *v = &g_vmc.items[idx];
        char sz[16]; human_size(v->size, sz, sizeof(sz));
        char line[200];
        snprintf(line, sizeof(line), " %-*.*s %7s", namew, namew, v->filename, sz);
        ui_text_hl(top + i, idx == g_vmc_sel, UI_WHITE, "%s", line);
    }
}

/* ---- MemCard Pro GC GameID switch ---- */

static void mcp_gameid(int port, const char *gamecode, const char *company, const char *name) {
    if (!g_state.mmce_mode[port]) {
        ui_error("Slot %c GameID off (enable in Config)", 'A' + port);
        return;
    }
    char msg[128];
    int rc = saves_mcp_set_gameid(port, gamecode, company, name, msg, sizeof(msg));
    if (rc < 0) ui_error("%s", msg); else ui_status("%s", msg);
    redraw();
}

static const char *hint_for(AppView v) {
    switch (v) {
        case APP_VIEW_CONFIG:    return "Up/Dn=select  L/R=change  A=edit/save        L+R+Start=quit";
        case APP_VIEW_ROMS:      return "A=fetch  X=queue  Y=download now  L/R=view    L+R+Start=quit";
        case APP_VIEW_LOCAL:     return "A=rescan  X=delete  L/R=switch view           L+R+Start=quit";
        case APP_VIEW_DOWNLOADS: return "A=start/resume  X=remove  (B=pause)           L+R+Start=quit";
        case APP_VIEW_SAVES:     return "A=rescan  X=import card  Y=pull all GCIs       L+R+Start=quit";
        case APP_VIEW_CARDA:
        case APP_VIEW_CARDB:     return "A=upload  Y=restore  Z=GameID  X=rescan        L+R+Start=quit";
        case APP_VIEW_SERVER:    return "A=restore->A  Y=->B  Z=GameID  X=refresh        L+R+Start=quit";
        default:                 return "L/R=switch view                               L+R+Start=quit";
    }
}

static void redraw(void) {
    ui_clear();
    ui_draw_header(&g_state, g_view);
    switch (g_view) {
        case APP_VIEW_ROMS:      draw_roms(); break;
        case APP_VIEW_LOCAL:     draw_local(); break;
        case APP_VIEW_DOWNLOADS: draw_downloads(); break;
        case APP_VIEW_SAVES:     draw_vmc(); break;
        case APP_VIEW_CARDA:     draw_card(&g_carda, g_ca_sel, g_ca_scroll); break;
        case APP_VIEW_CARDB:     draw_card(&g_cardb, g_cb_sel, g_cb_scroll); break;
        case APP_VIEW_SERVER:    draw_server(g_sv_sel, g_sv_scroll); break;
        case APP_VIEW_CONFIG:    draw_config(); break;
        default: break;
    }
    ui_draw_footer(hint_for(g_view));
    ui_flush();
}

static void cycle_view(int delta) {
    int n = (int)APP_VIEW_COUNT;
    g_view = (AppView)((((int)g_view + delta) % n + n) % n);
}

/* ---- main ---- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* VIDEO must come up before PAD: libogc's pad sampling is configured
     * from the active video mode.  PAD_Init() first works on Dolphin but
     * leaves the controller dead on real hardware. */
    ui_init();
    PAD_Init();

    show_boot("Mounting SD card...");
    mount_sd(&g_state);

    char err[256] = {0};
    config_load(&g_state, err, sizeof(err));
    config_load_console_id(&g_state);
    roms_set_target(g_state.sd_root, g_state.games_folder);

    show_boot("Bringing up network (BBA)...");
    network_init(&g_state);

    CARD_Init("GCSY", "8B");

    if (g_state.sd_ready) {
        roms_ensure_target_dirs();
        downloads_load(&g_downloads);
    }

    if (!g_state.sd_ready)
        ui_error("No SD card found (ROM install / VMC disabled)");
    else if (network_is_ready(&g_state))
        ui_status("Ready - ip %s, sd %s", g_state.ip, sd_device_to_str(g_state.sd_device));
    else
        ui_status("SD ready (%s); network not up", sd_device_to_str(g_state.sd_device));

    g_view = APP_VIEW_ROMS;
    if (g_state.sd_ready) { scan_local(); saves_scan_vmc(&g_vmc); }
    scan_card_view(0, &g_carda);
    scan_card_view(1, &g_cardb);
    if (network_is_ready(&g_state)) { fetch_catalog(); fetch_server(); }

    redraw();

    bool running = true;
    while (running) {
        /* Pace the loop at vsync and only redraw on input.  An unthrottled
         * redraw-every-iteration loop outruns gxflux's frame lifecycle and
         * wedges the GX FIFO on real hardware (screen freezes, input dead). */
        VIDEO_WaitVSync();
        PAD_ScanPads();
        u32 down = PAD_ButtonsDown(0);
        u32 held = PAD_ButtonsHeld(0);

        const u32 quit_combo = PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_BUTTON_START;
        if ((held & quit_combo) == quit_combo) { running = false; }

        else if (down == 0) continue;   /* nothing pressed - keep last frame */

        else if (down & PAD_TRIGGER_L) cycle_view(-1);
        else if (down & PAD_TRIGGER_R) cycle_view(+1);

        else if (g_view == APP_VIEW_ROMS) {
            int c = g_catalog.count;
            if      (down & PAD_BUTTON_UP)    g_rom_sel--;
            else if (down & PAD_BUTTON_DOWN)  g_rom_sel++;
            else if (down & PAD_BUTTON_LEFT)  g_rom_sel -= ui_list_visible();
            else if (down & PAD_BUTTON_RIGHT) g_rom_sel += ui_list_visible();
            else if (down & PAD_BUTTON_A)     fetch_catalog();
            else if (down & PAD_BUTTON_X)     queue_selected_rom(false);
            else if (down & PAD_BUTTON_Y)     queue_selected_rom(true);
            clamp_scroll(&g_rom_sel, &g_rom_scroll, c);
        }
        else if (g_view == APP_VIEW_LOCAL) {
            int c = g_local.count;
            if      (down & PAD_BUTTON_UP)    g_local_sel--;
            else if (down & PAD_BUTTON_DOWN)  g_local_sel++;
            else if (down & PAD_BUTTON_LEFT)  g_local_sel -= ui_list_visible();
            else if (down & PAD_BUTTON_RIGHT) g_local_sel += ui_list_visible();
            else if (down & PAD_BUTTON_A)     scan_local();
            else if (down & PAD_BUTTON_X) {
                if (c > 0 && g_local_sel < c) {
                    if (unlink(g_local.items[g_local_sel].path) == 0) {
                        ui_status("Deleted: %s", g_local.items[g_local_sel].filename);
                        scan_local();
                    } else ui_error("Delete failed");
                }
            }
            clamp_scroll(&g_local_sel, &g_local_scroll, g_local.count);
        }
        else if (g_view == APP_VIEW_DOWNLOADS) {
            int c = g_downloads.count;
            if      (down & PAD_BUTTON_UP)    g_dl_sel--;
            else if (down & PAD_BUTTON_DOWN)  g_dl_sel++;
            else if (down & PAD_BUTTON_A) {
                if (c > 0 && g_dl_sel < c) run_active_download(&g_downloads.items[g_dl_sel]);
            }
            else if (down & PAD_BUTTON_X) {
                if (c > 0 && g_dl_sel < c) {
                    downloads_remove(&g_downloads, g_downloads.items[g_dl_sel].rom_id);
                    downloads_save(&g_downloads);
                }
            }
            clamp_scroll(&g_dl_sel, &g_dl_scroll, g_downloads.count);
        }
        else if (g_view == APP_VIEW_SAVES) {
            if      (down & PAD_BUTTON_UP)    g_vmc_sel--;
            else if (down & PAD_BUTTON_DOWN)  g_vmc_sel++;
            else if (down & PAD_BUTTON_LEFT)  g_vmc_sel -= ui_list_visible();
            else if (down & PAD_BUTTON_RIGHT) g_vmc_sel += ui_list_visible();
            else if (down & PAD_BUTTON_A)     scan_vmc_view();
            else if (down & PAD_BUTTON_X)     upload_vmc_at(g_vmc_sel);
            else if (down & PAD_BUTTON_Y)     pull_all_vmc();
            clamp_scroll(&g_vmc_sel, &g_vmc_scroll, g_vmc.count);
        }
        else if (g_view == APP_VIEW_CARDA) {
            if      (down & PAD_BUTTON_UP)    g_ca_sel--;
            else if (down & PAD_BUTTON_DOWN)  g_ca_sel++;
            else if (down & PAD_BUTTON_LEFT)  g_ca_sel -= ui_list_visible();
            else if (down & PAD_BUTTON_RIGHT) g_ca_sel += ui_list_visible();
            else if (down & PAD_BUTTON_X)     scan_card_view(0, &g_carda);
            else if (down & PAD_BUTTON_A)     upload_card_at(&g_carda, g_ca_sel);
            else if (down & PAD_BUTTON_Y)     restore_card_at(&g_carda, g_ca_sel);
            else if (down & PAD_TRIGGER_Z) {
                if (g_carda.count > 0 && g_ca_sel < g_carda.count) {
                    const GcSave *s = &g_carda.items[g_ca_sel];
                    mcp_gameid(0, s->gamecode, s->company, s->name[0] ? s->name : s->title_id);
                }
            }
            clamp_scroll(&g_ca_sel, &g_ca_scroll, g_carda.count);
        }
        else if (g_view == APP_VIEW_CARDB) {
            if      (down & PAD_BUTTON_UP)    g_cb_sel--;
            else if (down & PAD_BUTTON_DOWN)  g_cb_sel++;
            else if (down & PAD_BUTTON_LEFT)  g_cb_sel -= ui_list_visible();
            else if (down & PAD_BUTTON_RIGHT) g_cb_sel += ui_list_visible();
            else if (down & PAD_BUTTON_X)     scan_card_view(1, &g_cardb);
            else if (down & PAD_BUTTON_A)     upload_card_at(&g_cardb, g_cb_sel);
            else if (down & PAD_BUTTON_Y)     restore_card_at(&g_cardb, g_cb_sel);
            else if (down & PAD_TRIGGER_Z) {
                if (g_cardb.count > 0 && g_cb_sel < g_cardb.count) {
                    const GcSave *s = &g_cardb.items[g_cb_sel];
                    mcp_gameid(1, s->gamecode, s->company, s->name[0] ? s->name : s->title_id);
                }
            }
            clamp_scroll(&g_cb_sel, &g_cb_scroll, g_cardb.count);
        }
        else if (g_view == APP_VIEW_SERVER) {
            if      (down & PAD_BUTTON_UP)    g_sv_sel--;
            else if (down & PAD_BUTTON_DOWN)  g_sv_sel++;
            else if (down & PAD_BUTTON_LEFT)  g_sv_sel -= ui_list_visible();
            else if (down & PAD_BUTTON_RIGHT) g_sv_sel += ui_list_visible();
            else if (down & PAD_BUTTON_X)     fetch_server();
            else if (down & PAD_BUTTON_A)     server_restore_to(0);
            else if (down & PAD_BUTTON_Y)     server_restore_to(1);
            else if (down & PAD_TRIGGER_Z) {
                if (g_server.count > 0 && g_sv_sel < g_server.count) {
                    const ServerSave *s = &g_server.items[g_sv_sel];
                    const char *gc = strncmp(s->title_id, "GC_", 3) == 0 ? s->title_id + 3 : s->title_id;
                    int port = g_state.mmce_mode[0] ? 0 : (g_state.mmce_mode[1] ? 1 : 0);
                    mcp_gameid(port, gc, "", s->name[0] ? s->name : s->title_id);
                }
            }
            clamp_scroll(&g_sv_sel, &g_sv_scroll, g_server.count);
        }
        else if (g_view == APP_VIEW_CONFIG) config_input(down);

        redraw();
    }

    network_shutdown();
    return 0;
}
