/*
 * PS2 Save Sync — main entry.
 *
 * Boot order:
 *   1. SifInitRpc, kernel + IOP setup
 *   2. Reset IOP and load embedded IRX modules:
 *        sio2man, mcman, mcserv, padman   — controllers + memcards
 *        usbd, usbhdfsd                   — USB mass storage (mass:/)
 *        ps2dev9, netman, smap            — network adapter
 *   3. Initialise libdebug screen
 *   4. Load config from mc0:/3DSSYNC/CONFIG.TXT
 *   5. Bring up networking via ps2ip (static IP by default, DHCP optional)
 *   6. Probe mass:/ (USB is required for downloads, not catalog browse)
 *   7. Run the menu loop (ROMs / Downloads / Config views)
 *
 * Phase 1 only: ROM browser + USB installer.  Save sync (MCP2) is
 * stubbed in the menu - the next phase wires it up.
 */

#include "common.h"
#include "config.h"
#include "downloads.h"
#include "http.h"
#include "network.h"
#include "roms.h"
#include "ui.h"
#include "irx_mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <libpad.h>
#include <libmc.h>
#include <debug.h>
#include <delaythread.h>
#include <netman.h>
#include <ps2ip.h>
#include <sbv_patches.h>

/* fileXio_rpc.h is gated by NEWLIB_PORT_AWARE — defining it before
 * inclusion is the supported way to call fileXioInit() from a newlib
 * project.  We need that call: even with fileXio.irx loaded on the IOP
 * side, the EE-side stub never binds its RPC descriptor until
 * fileXioInit() is invoked, and any fopen/stat on a bdm-mounted volume
 * (mass:/, hdd0:/, etc.) silently returns ENODEV. */
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

/* ---- IRX loading helpers ---- */

/* Returns 0 on success, negative on failure.
 *
 * SifExecModuleBuffer return values (per ps2lib_err.h):
 *   >= 0  module id, success
 *   -200  E_IOP_DEPENDANCY   — depends on a module that didn't load
 *   -201  E_LF_NOT_IRX       — buffer not a valid IRX (often misaligned)
 *   -203  E_LF_FILE_NOT_FOUND
 *   -204  E_LF_FILE_IO_ERROR
 *
 * `ret` is the IRX `_start` return value (MODULE_RESIDENT_END=0,
 * MODULE_NO_RESIDENT_END=1, or a module-defined error). */
static int load_irx(const unsigned char *blob, unsigned int size,
                    const char *label,
                    int argc, const char *argv)
{
    int ret = 0;
    int id = SifExecModuleBuffer((void *)blob, size, argc,
                                 (char *)argv, &ret);
    if (id < 0) {
        scr_printf("  IRX %s failed (id=%d ret=%d)\n", label, id, ret);
        return id;
    }
    if (ret != 0 && ret != 1) {
        scr_printf("  IRX %s start error (id=%d ret=%d)\n", label, id, ret);
        return -1;
    }
    scr_printf("  IRX %s ok (id=%d)\n", label, id);
    return 0;
}

static void boot_iop_modules(void) {
    /* SifInitRpc must run before SifIopReset — establishes the EE-side
     * RPC state that SifIopSync waits on. */
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}

    /* Re-establish RPC after reset; bring up the SIF heap so loaded
     * IRX modules can allocate from IOP RAM. */
    SifInitRpc(0);
    SifInitIopHeap();
    SifLoadFileInit();

    /* sbv patches let unsigned IRX run from EE memory. */
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    scr_printf("Loading IOP modules...\n");

    /* iomanX + fileXio first — every block-device IRX (bdm, usbmass_bd,
     * bdmfs_fatfs) registers its mountpoint with iomanX, and newlib's
     * stdio routes through fileXio. */
    load_irx(iomanX_irx,      iomanX_irx_size,      "iomanX",      0, NULL);
    load_irx(fileXio_irx,     fileXio_irx_size,     "fileXio",     0, NULL);

    /* SIO2/memcard/pad load from Sony's IOP ROM (rom0:) — the PS2SDK
     * mcman.irx / mcserv.irx have a different RPC ABI and cause libmc
     * mcInit to hang. rom0: modules are guaranteed present on every
     * PS2 and use the original Sony RPC numbers libmc expects. */
    int ret;
    ret = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    scr_printf("  rom0:SIO2MAN -> %d\n", ret);
    ret = SifLoadModule("rom0:MCMAN",   0, NULL);
    scr_printf("  rom0:MCMAN   -> %d\n", ret);
    ret = SifLoadModule("rom0:MCSERV",  0, NULL);
    scr_printf("  rom0:MCSERV  -> %d\n", ret);
    ret = SifLoadModule("rom0:PADMAN",  0, NULL);
    scr_printf("  rom0:PADMAN  -> %d\n", ret);

    /* USB mass storage stack.  Two competing options:
     *   (a) Modern split: bdm + usbmass_bd + bdmfs_fatfs
     *       bdmfs_fatfs registers mass: once a USB device with a FAT
     *       partition attaches.
     *   (b) Legacy: usbhdfsd — single module, no bdm dependency.
     *
     * Some launchers (and PCSX2) preload an old bdm.irx that's binary
     * incompatible with our newer usbmass_bd.irx, causing usbmass_bd to
     * fail with id=-200 (unresolved symbol).  When that happens we fall
     * back to usbhdfsd, which has no external dependencies. */
    load_irx(usbd_irx,        usbd_irx_size,        "usbd",        0, NULL);

    int bdm_rc        = load_irx(bdm_irx,         bdm_irx_size,         "bdm",         0, NULL);
    int usbmass_rc    = load_irx(usbmass_bd_irx,  usbmass_bd_irx_size,  "usbmass_bd",  0, NULL);
    int bdmfs_rc      = load_irx(bdmfs_fatfs_irx, bdmfs_fatfs_irx_size, "bdmfs_fatfs", 0, NULL);

    if (bdm_rc != 0 || usbmass_rc != 0 || bdmfs_rc != 0) {
        scr_printf("  bdm stack failed - falling back to usbhdfsd\n");
        load_irx(usbhdfsd_irx, usbhdfsd_irx_size, "usbhdfsd", 0, NULL);
    }

    load_irx(ps2dev9_irx,     ps2dev9_irx_size,     "ps2dev9",     0, NULL);
    load_irx(netman_irx,      netman_irx_size,      "netman",      0, NULL);
    load_irx(smap_irx,        smap_irx_size,        "smap",        0, NULL);

    /* Bind the EE-side fileXio stub.  Without this, every newlib stdio
     * call against an iomanX-registered device (mass:, hdd:, etc.)
     * silently fails — the RPC descriptor is uninitialised. */
    int fxr = fileXioInit();
    scr_printf("  fileXioInit -> %d\n", fxr);
}

/* ---- mass:/ wait ---- */

static bool wait_for_mass(SyncState *state, int timeout_seconds) {
    /* USB enumeration is asynchronous — wait until a ``mass:`` root is
     * usable. Some PS2 USB stacks reject fopen() on a directory, so we
     * probe by creating the app data directory on each possible root.
     *
     * Both usbhdfsd and bdmfs_fatfs register the iomanX driver name
     * "mass"; iomanX accepts "mass:" or "massN:" (with N = unit id), so
     * we try a handful of variants before giving up. */
    static const char *roots[] = {
        "mass:", "mass0:", "mass1:", "mass2:", "mass3:", NULL
    };

    state->usb_ready = false;
    strncpy(state->usb_root, USB_DEFAULT_ROOT, sizeof(state->usb_root) - 1);
    state->usb_root[sizeof(state->usb_root) - 1] = '\0';
    roms_set_usb_root(state->usb_root);

    int last_errno[8] = {0};

    for (int i = 0; i < timeout_seconds * 4; i++) {
        for (int r = 0; roots[r]; r++) {
            char data_dir[96];
            char root_dir[32];
            snprintf(data_dir, sizeof(data_dir), "%s%s",
                     roots[r], USB_DATA_SUBDIR);
            snprintf(root_dir, sizeof(root_dir), "%s/", roots[r]);

            errno = 0;
            struct stat st;
            int stat_rc = stat(data_dir, &st);
            int stat_errno = errno;

            if (stat_rc == 0) {
                scr_printf("  USB: stat ok at %s\n", data_dir);
                strncpy(state->usb_root, roots[r], sizeof(state->usb_root) - 1);
                state->usb_root[sizeof(state->usb_root) - 1] = '\0';
                state->usb_ready = true;
                roms_set_usb_root(state->usb_root);
                return true;
            }

            errno = 0;
            int mkdir_rc = mkdir(data_dir, 0777);
            int mkdir_errno = errno;
            if (mkdir_rc == 0 || mkdir_errno == EEXIST) {
                scr_printf("  USB: mkdir ok at %s\n", data_dir);
                strncpy(state->usb_root, roots[r], sizeof(state->usb_root) - 1);
                state->usb_root[sizeof(state->usb_root) - 1] = '\0';
                state->usb_ready = true;
                roms_set_usb_root(state->usb_root);
                return true;
            }

            errno = 0;
            if (stat(root_dir, &st) == 0) {
                scr_printf("  USB: root stat ok at %s — using anyway\n", root_dir);
                mkdir(data_dir, 0777);
                strncpy(state->usb_root, roots[r], sizeof(state->usb_root) - 1);
                state->usb_root[sizeof(state->usb_root) - 1] = '\0';
                state->usb_ready = true;
                roms_set_usb_root(state->usb_root);
                return true;
            }

            last_errno[r] = mkdir_errno ? mkdir_errno : stat_errno;
        }
        DelayThread(250000);
    }

    /* Show why we gave up — last errno from each candidate root. */
    scr_printf("  USB: wait_for_mass timed out after %ds\n", timeout_seconds);
    for (int r = 0; roots[r]; r++) {
        scr_printf("    %-7s last errno=%d\n", roots[r], last_errno[r]);
    }
    return false;
}

/* ---- Pad input ---- */

static char g_pad_buf[256] __attribute__((aligned(64)));
static struct padButtonStatus g_pad_state;

static void pad_init(void) {
    padInit(0);
    padPortOpen(0, 0, g_pad_buf);
    padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
}

static unsigned int g_prev_btns = 0;

static unsigned int pad_read_pressed(void) {
    /* Non-blocking: bail this frame if the pad isn't in a stable state.
     * The previous version spun on padGetState() until STABLE, which
     * deadlocks the main loop when the pad RPC stalls (seen post-gsKit
     * init). Returning 0 here just means "no input this tick" — the
     * loop will retry next iteration. */
    int state = padGetState(0, 0);
    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
        return 0;
    }
    if (padRead(0, 0, &g_pad_state) == 0) return 0;
    unsigned int btns = 0xFFFF ^ g_pad_state.btns;
    unsigned int pressed = btns & ~g_prev_btns;
    g_prev_btns = btns;
    return pressed;
}

/* ---- App state ---- */

/* Forward declaration: fetch_catalog / scan_local / run_active_download
 * call redraw() so long-running operations flush their "started" /
 * "finished" status messages to the screen instead of leaving the
 * previous frame on screen until the next button press. */
static void redraw(void);

static SyncState     g_state;
static RomCatalog    g_catalog;
static LocalRomList  g_local;
static DownloadList  g_downloads;
static AppView       g_view = APP_VIEW_ROMS;
static int           g_rom_selected   = 0, g_rom_scroll   = 0;
static int           g_local_selected = 0, g_local_scroll = 0;
static int           g_dl_selected    = 0, g_dl_scroll    = 0;

static char          g_scratch[256 * 1024];      /* JSON page buffer */

static bool require_usb_ready(void) {
    if (!g_state.usb_ready) {
        ui_error("USB not ready; catalog browse still works");
        return false;
    }
    return true;
}

static void init_usb_targets(void) {
    scr_printf("BOOT: probing USB mass storage...\n");
    if (!wait_for_mass(&g_state, 12)) {
        scr_printf("WARN: no USB mass storage mounted\n");
        ui_status("USB not ready; downloads disabled");
        return;
    }

    scr_printf("BOOT: USB ready at %s\n", g_state.usb_root);
    roms_ensure_target_dirs();
    downloads_load(&g_downloads);
}

/* Live progress from network_set_progress64_cb. */
static volatile uint64_t g_active_done  = 0;
static volatile uint64_t g_active_total = 0;
static volatile uint64_t g_active_bps   = 0;
static volatile bool     g_pause_requested = false;
static time_t            g_last_speed_t = 0;
static uint64_t          g_last_speed_bytes = 0;

static int progress_cb(uint64_t done, uint64_t total) {
    g_active_done  = done;
    g_active_total = total;

    time_t now = time(NULL);
    if (now != g_last_speed_t) {
        g_active_bps = done > g_last_speed_bytes
                     ? (done - g_last_speed_bytes) / (uint64_t)(now - g_last_speed_t + 1)
                     : 0;
        /* Refresh the screen at most once per wall-clock second so the
         * progress bar / KB/s line update without flooding libdebug
         * with full-frame char DMA. */
        redraw();
        g_last_speed_t     = now;
        g_last_speed_bytes = done;
    }

    return g_pause_requested ? 1 : 0;
}

/* ---- View handlers ---- */

static void clamp_scroll(int *selected, int *scroll, int count) {
    if (count == 0) { *selected = 0; *scroll = 0; return; }
    if (*selected < 0) *selected = 0;
    if (*selected >= count) *selected = count - 1;
    int visible = ui_list_visible();
    if (visible < 1) visible = 1;
    if (*selected < *scroll) *scroll = *selected;
    if (*selected >= *scroll + visible) *scroll = *selected - visible + 1;
    if (*scroll < 0) *scroll = 0;
}

static void fetch_catalog(void) {
    ui_status("Fetching PS2 catalog...");
    redraw();   /* show "Fetching..." before HTTP blocks the loop */
    bool ok = roms_fetch_catalog(&g_state, "PS2",
                                 g_scratch, sizeof(g_scratch),
                                 &g_catalog);
    if (!ok) {
        ui_error("%s", g_catalog.last_error);
    } else {
        ui_status("Catalog: %d entries", g_catalog.count);
    }
    redraw();   /* show result */
}

static void scan_local(void) {
    if (!g_state.usb_ready) {
        snprintf(g_local.last_error, sizeof(g_local.last_error),
                 "USB not ready");
        g_local.count = 0;
        return;
    }
    ui_status("Scanning local ISOs...");
    redraw();
    roms_scan_local(&g_local);
    if (g_local.count > 0) {
        ui_status("Local: %d ISOs", g_local.count);
    }
    redraw();
}

static void run_active_download(DownloadEntry *e) {
    if (!e || !require_usb_ready()) return;

    g_active_done       = e->offset;
    g_active_total      = e->total;
    g_pause_requested   = false;
    g_last_speed_t      = time(NULL);
    g_last_speed_bytes  = e->offset;

    network_set_progress64_cb(progress_cb);

    /* Make sure the destination dir exists. */
    char dir[256];
    strncpy(dir, e->target_path, sizeof(dir));
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        roms_mkdir_p(dir);
    }

    e->status = DL_STATUS_ACTIVE;
    downloads_save(&g_downloads);

    uint64_t total = 0;
    int rc = network_download_rom_resumable(&g_state, e->rom_id,
                                            e->extract_format,
                                            e->target_path,
                                            e->offset,
                                            &total);
    network_set_progress64_cb(NULL);

    if (rc == 0) {
        e->status = DL_STATUS_COMPLETED;
        e->offset = total > 0 ? total : g_active_done;
        ui_status("Done: %s", e->name);
    } else if (rc == 1) {
        e->status = DL_STATUS_PAUSED;
        e->offset = g_active_done;
        ui_status("Paused: %s", e->name);
    } else {
        e->status = DL_STATUS_ERROR;
        e->offset = g_active_done;
        ui_error("Download failed (rc=%d)", rc);
    }
    downloads_save(&g_downloads);

    g_active_total = 0;
    redraw();   /* surface the result line so the user can read it */
}

/* ---- Main loop ---- */

static void cycle_view(void) {
    g_view = (AppView)(((int)g_view + 1) % APP_VIEW_COUNT);
}

static void redraw(void) {
    ui_clear();
    ui_draw_header(&g_state, g_view);
    switch (g_view) {
        case APP_VIEW_ROMS:
            ui_draw_roms(&g_catalog, g_rom_selected, g_rom_scroll);
            break;
        case APP_VIEW_LOCAL:
            ui_draw_local(&g_local, g_local_selected, g_local_scroll);
            break;
        case APP_VIEW_DOWNLOADS:
            ui_draw_downloads(&g_downloads, g_dl_selected, g_dl_scroll,
                              g_active_done, g_active_total, g_active_bps);
            break;
        case APP_VIEW_CONFIG:
            ui_draw_config(&g_state);
            break;
        default: break;
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    /* Phase 1: bring up the libdebug scrolling console so every boot
     * step (IRX load, mc init, network bring-up) leaves a visible
     * line.  gsKit isn't initialised yet — that happens after boot
     * completes, in the ui_init() call below. */
    ui_boot_init();
    scr_printf("ps2sync v%s booting...\n", APP_VERSION);

    boot_iop_modules();
    scr_printf("BOOT: IRX done — pausing 3 s so you can read the log\n");
    DelayThread(3000000);

    scr_printf("BOOT: loading config (mcInit...)\n");
    char err[256];
    if (!config_load(&g_state, err, sizeof(err))) {
        ui_draw_message("Config", err);
        scr_printf("\nPress CIRCLE to exit.\n");
        pad_init();
        for (;;) {
            unsigned int p = pad_read_pressed();
            if (p & PAD_CIRCLE) break;
        }
        return 1;
    }
    config_load_console_id(&g_state);
    scr_printf("BOOT: console_id=%s server=%s\n",
               g_state.console_id, g_state.server_url);

    scr_printf("BOOT: bringing up network\n");
    network_init(&g_state);
    scr_printf("BOOT: net ready=%d dhcp=%d ip=%s\n",
               g_state.net_ready, g_state.dhcp_ok, g_state.ip);

    init_usb_targets();

    scr_printf("BOOT: pad init\n");
    pad_init();

    scr_printf("BOOT: ready - switching to gsKit UI\n");
    /* Pause briefly so user sees boot log before gsKit overwrites it. */
    DelayThread(2000000);

    /* Phase 2: hand the GS to gsKit. */
    ui_init();

    /* Auto-populate the views so the user lands on a usable list
     * instead of "Press X to fetch catalog" / "No ISOs". */
    if (g_state.usb_ready) {
        scan_local();
    }
    if (network_is_ready(&g_state)) {
        fetch_catalog();
    }

    redraw();
    ui_flush();

    for (;;) {
        unsigned int pressed = pad_read_pressed();

        /* Event-driven redraw: libdebug renders synchronously into the
         * GS frame buffer with no double buffer, so repainting on every
         * frame produces visible flicker (~140 KB of char DMA per
         * frame).  Only redraw when state actually changed. */
        if (pressed == 0) {
            DelayThread(16000);
            continue;
        }

        bool dirty = true;

        if (pressed & PAD_CIRCLE) break;

        if (pressed & PAD_START) {
            cycle_view();
        } else if (g_view == APP_VIEW_ROMS) {
            int count = g_catalog.count;
            if      (pressed & PAD_UP)       g_rom_selected--;
            else if (pressed & PAD_DOWN)     g_rom_selected++;
            else if (pressed & PAD_LEFT)     g_rom_selected -= ui_list_visible();
            else if (pressed & PAD_RIGHT)    g_rom_selected += ui_list_visible();
            else if (pressed & PAD_CROSS)    fetch_catalog();
            else if (pressed & PAD_SQUARE) {
                /* Queue selected ROM for download. */
                if (require_usb_ready() &&
                    count > 0 && g_rom_selected < count)
                {
                    DownloadEntry *e = downloads_upsert_from_catalog(
                        &g_downloads, &g_catalog.items[g_rom_selected]);
                    if (e) {
                        downloads_save(&g_downloads);
                        ui_status("Queued: %s", e->name);
                    } else {
                        ui_error("Download list full");
                    }
                }
            } else if (pressed & PAD_TRIANGLE) {
                /* Trigger active download for the selected entry. */
                if (require_usb_ready() &&
                    count > 0 && g_rom_selected < count)
                {
                    DownloadEntry *e = downloads_upsert_from_catalog(
                        &g_downloads, &g_catalog.items[g_rom_selected]);
                    if (e) run_active_download(e);
                }
            }
            clamp_scroll(&g_rom_selected, &g_rom_scroll, count);
        } else if (g_view == APP_VIEW_LOCAL) {
            int count = g_local.count;
            if      (pressed & PAD_UP)       g_local_selected--;
            else if (pressed & PAD_DOWN)     g_local_selected++;
            else if (pressed & PAD_LEFT)     g_local_selected -= ui_list_visible();
            else if (pressed & PAD_RIGHT)    g_local_selected += ui_list_visible();
            else if (pressed & PAD_CROSS)    scan_local();
            else if (pressed & PAD_SQUARE) {
                /* Delete the selected installed ISO. */
                if (count > 0 && g_local_selected < count) {
                    const LocalRom *r = &g_local.items[g_local_selected];
                    if (unlink(r->path) == 0) {
                        ui_status("Deleted: %s", r->filename);
                        scan_local();
                    } else {
                        ui_error("Delete failed: %s", r->filename);
                    }
                }
            }
            clamp_scroll(&g_local_selected, &g_local_scroll, count);
        } else if (g_view == APP_VIEW_DOWNLOADS) {
            int count = g_downloads.count;
            if      (pressed & PAD_UP)       g_dl_selected--;
            else if (pressed & PAD_DOWN)     g_dl_selected++;
            else if (pressed & PAD_CROSS) {
                if (count > 0 && g_dl_selected < count) {
                    run_active_download(&g_downloads.items[g_dl_selected]);
                }
            } else if (pressed & PAD_SQUARE) {
                if (require_usb_ready() &&
                    count > 0 && g_dl_selected < count)
                {
                    downloads_remove(&g_downloads,
                                     g_downloads.items[g_dl_selected].rom_id);
                    downloads_save(&g_downloads);
                    count = g_downloads.count;
                }
            }
            clamp_scroll(&g_dl_selected, &g_dl_scroll, count);
        }

        if (dirty) redraw();
    }

    network_shutdown();
    return 0;
}
