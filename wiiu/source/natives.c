/*
 * natives.c — vWii (SLC) and Wii U (MLC) save enumeration via libmocha.
 *
 * Mocha_MountFS() registers a newlib devoptab for a raw device, so once the
 * mounts are up the NAND trees are ordinary POSIX paths and savetree.c does
 * the walking.
 *
 * Writes are deliberately confined to the save directories themselves, and
 * every restore is preceded by an SD backup — a bad write to SLC is a
 * brick-adjacent event.
 */

#include "natives.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <mocha/mocha.h>

#define SLC_NAME  "storage_slccmpt01"
#define MLC_NAME  "storage_mlc01"
#define USB_NAME  "storage_usb01"

#define VWII_TITLES_DIR SLC_NAME ":/title/00010000"
#define WIIU_SAVES_DIR  MLC_NAME ":/usr/save/00050000"
#define WIIU_USB_SAVES  USB_NAME ":/usr/save/00050000"

/* A title installed to USB keeps its save data on that USB drive, so the
 * Wii U scan has to cover both roots.  MLC is always present; USB only when
 * a drive is attached and mountable. */
static const char *const WIIU_SAVE_ROOTS[] = {
    WIIU_SAVES_DIR,
    WIIU_USB_SAVES,
    NULL,
};

/* nocopy/ holds console-bound data that must never be moved between
 * consoles; nobackup/ is game scratch that the Wii itself excludes. */
static const char *const VWII_EXCLUDE[] = { "nocopy", NULL };

/* Treat "already mounted under this name" as success — re-entering the app
 * without a reboot leaves the previous devoptab registered. */
static MochaUtilsStatus mount_fs(const char *name, const char *dev,
                                 const char *mount_path) {
    MochaUtilsStatus st = Mocha_MountFS(name, dev, mount_path);
    if (st == MOCHA_RESULT_ALREADY_EXISTS) return MOCHA_RESULT_SUCCESS;
    return st;
}

int natives_init(SyncState *state) {
    if (!state) return -1;
    state->mocha_ok = false;
    state->slc_mounted = false;
    state->mlc_mounted = false;
    state->mocha_error[0] = '\0';

    MochaUtilsStatus st = Mocha_InitLibrary();
    if (st != MOCHA_RESULT_SUCCESS) {
        snprintf(state->mocha_error, sizeof(state->mocha_error),
                 "mocha init: %s", Mocha_GetStatusStr(st));
        return -1;
    }
    state->mocha_ok = true;

    /* Mocha_MountFS's dev_path is "mount this device", and passing NULL means
     * "attach to the mount CafeOS already has".  Which one is right differs
     * per device:
     *
     *   MLC  is mounted by the OS at boot, so a fresh /dev/mlc01 mount onto
     *        the same /vol path yields a devoptab whose opendir() fails —
     *        that was the "cannot open storage_mlc01:/usr/save/00050000".
     *   SLC (vWii compat) is NOT mounted in Wii U mode, so it needs the
     *        explicit device path.
     *
     * Try the likely form first and fall back to the other, so this keeps
     * working if a future CFW changes what is pre-mounted. */
    st = mount_fs(SLC_NAME, "/dev/slccmpt01", "/vol/" SLC_NAME);
    if (st != MOCHA_RESULT_SUCCESS)
        st = mount_fs(SLC_NAME, NULL, "/vol/" SLC_NAME);
    if (st == MOCHA_RESULT_SUCCESS) {
        state->slc_mounted = true;
    } else {
        snprintf(state->mocha_error, sizeof(state->mocha_error),
                 "slc mount: %s", Mocha_GetStatusStr(st));
    }

    st = mount_fs(MLC_NAME, NULL, "/vol/storage_mlc01");
    if (st != MOCHA_RESULT_SUCCESS)
        st = mount_fs(MLC_NAME, "/dev/mlc01", "/vol/" MLC_NAME);
    if (st == MOCHA_RESULT_SUCCESS) {
        state->mlc_mounted = true;
    } else if (!state->mocha_error[0]) {
        snprintf(state->mocha_error, sizeof(state->mocha_error),
                 "mlc mount: %s", Mocha_GetStatusStr(st));
    }

    /* Optional: only present when a USB drive is attached.  Failure is normal
     * and must not be reported as an error. */
    if (mount_fs(USB_NAME, NULL, "/vol/storage_usb01") == MOCHA_RESULT_SUCCESS)
        state->usb_mounted = true;

    return 0;
}

void natives_shutdown(SyncState *state) {
    if (!state || !state->mocha_ok) return;
    if (state->slc_mounted) Mocha_UnmountFS(SLC_NAME);
    if (state->mlc_mounted) Mocha_UnmountFS(MLC_NAME);
    if (state->usb_mounted) Mocha_UnmountFS(USB_NAME);
    Mocha_DeInitLibrary();
    state->mocha_ok = false;
    state->slc_mounted = false;
    state->mlc_mounted = false;
}

/* ---- helpers ---- */

static bool is_hex8(const char *s) {
    if (strlen(s) != 8) return false;
    for (int i = 0; i < 8; i++)
        if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

static bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ---- vWii ---- */

/* tidlo "524d4345" -> "RMCE" (printable ASCII only). */
static bool tidlo_to_ascii4(const char *tidlo, char *out5) {
    for (int i = 0; i < 4; i++) {
        char pair[3] = { tidlo[i * 2], tidlo[i * 2 + 1], 0 };
        long v = strtol(pair, NULL, 16);
        if (v < 0x20 || v > 0x7E) return false;
        out5[i] = (char)v;
    }
    out5[4] = '\0';
    return true;
}

static void ascii4_to_tidlo(const char *code4, char *out9) {
    snprintf(out9, 9, "%02x%02x%02x%02x",
             (unsigned char)code4[0], (unsigned char)code4[1],
             (unsigned char)code4[2], (unsigned char)code4[3]);
}

bool vwiisaves_root_for(const char *title_id, char *out, size_t out_size) {
    if (!title_id || strncmp(title_id, "WII_", 4) != 0) return false;
    const char *code = title_id + 4;
    if (strlen(code) != 4) return false;
    char tidlo[9];
    ascii4_to_tidlo(code, tidlo);
    snprintf(out, out_size, VWII_TITLES_DIR "/%s/data", tidlo);
    return true;
}

void vwiisaves_scan(const SyncState *state, SaveTitleList *out) {
    if (!out) return;
    savetree_free_list(out);
    out->title_count = 0;
    out->last_error[0] = '\0';

    if (!state->slc_mounted) {
        snprintf(out->last_error, sizeof(out->last_error),
                 "vWii NAND not mounted (%s)",
                 state->mocha_error[0] ? state->mocha_error : "mocha unavailable");
        return;
    }

    DIR *d = opendir(VWII_TITLES_DIR);
    if (!d) {
        snprintf(out->last_error, sizeof(out->last_error),
                 "Cannot open %s", VWII_TITLES_DIR);
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && out->title_count < SAVE_MAX_TITLES) {
        if (de->d_name[0] == '.') continue;
        if (!is_hex8(de->d_name)) continue;

        char code[5];
        if (!tidlo_to_ascii4(de->d_name, code)) continue;

        SaveTitle *t = &out->titles[out->title_count];
        memset(t, 0, sizeof(*t));
        snprintf(t->title_id, sizeof(t->title_id), "WII_%s", code);
        snprintf(t->root, sizeof(t->root), VWII_TITLES_DIR "/%s/data", de->d_name);
        if (!dir_exists(t->root)) continue;

        if (savetree_scan(t, VWII_EXCLUDE) != 0 || t->file_count == 0) {
            savetree_free_title(t);
            continue;
        }
        out->title_count++;
    }
    closedir(d);

    if (out->title_count == 0 && out->last_error[0] == '\0')
        snprintf(out->last_error, sizeof(out->last_error), "No vWii saves found");
}

/* ---- Wii U ---- */

bool wiiusaves_root_for(const char *title_id, char *out, size_t out_size) {
    if (!title_id || strlen(title_id) != 16) return false;
    if (strncasecmp(title_id, "00050000", 8) != 0) return false;

    /* Prefer whichever root actually holds this title (USB-installed games
     * keep their saves on the USB drive); fall back to MLC so a restore of a
     * server-only title still has somewhere to go. */
    for (int i = 0; WIIU_SAVE_ROOTS[i]; i++) {
        snprintf(out, out_size, "%s/%.8s/user", WIIU_SAVE_ROOTS[i], title_id + 8);
        if (dir_exists(out)) return true;
    }
    snprintf(out, out_size, WIIU_SAVES_DIR "/%.8s/user", title_id + 8);
    return true;
}

/* Pull <longname_en> (falling back to any <longname_*>) out of meta.xml.
 * The title's content lives next to its saves, so look on the same device. */
static void read_meta_longname(const char *device, const char *tidlo,
                               char *out, size_t out_size) {
    out[0] = '\0';
    char path[SAVE_DIR_LEN];
    snprintf(path, sizeof(path), "%s:/usr/title/00050000/%s/meta/meta.xml",
             device, tidlo);

    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return;
    buf[n] = '\0';

    const char *p = strstr(buf, "<longname_en");
    if (!p) p = strstr(buf, "<longname_");
    if (!p) return;
    p = strchr(p, '>');
    if (!p) return;
    p++;
    const char *end = strstr(p, "</longname");
    if (!end) return;

    size_t j = 0;
    for (const char *q = p; q < end && j + 1 < out_size; q++) {
        char c = *q;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && (j == 0 || out[j - 1] == ' ')) continue;
        out[j++] = c;
    }
    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';
}

void wiiusaves_scan(const SyncState *state, SaveTitleList *out) {
    if (!out) return;
    savetree_free_list(out);
    out->title_count = 0;
    out->last_error[0] = '\0';

    if (!state->mlc_mounted) {
        snprintf(out->last_error, sizeof(out->last_error),
                 "MLC not mounted (%s)",
                 state->mocha_error[0] ? state->mocha_error : "mocha unavailable");
        return;
    }

    int roots_opened = 0;
    for (int r = 0; WIIU_SAVE_ROOTS[r]; r++) {
        const char *root = WIIU_SAVE_ROOTS[r];
        /* The device prefix, e.g. "storage_mlc01", for the sibling meta.xml. */
        char device[32];
        const char *colon = strchr(root, ':');
        size_t dlen = colon ? (size_t)(colon - root) : strlen(root);
        if (dlen >= sizeof(device)) dlen = sizeof(device) - 1;
        memcpy(device, root, dlen);
        device[dlen] = '\0';

        DIR *d = opendir(root);
        if (!d) continue;     /* USB simply may not be attached */
        roots_opened++;

        struct dirent *de;
        while ((de = readdir(d)) != NULL && out->title_count < SAVE_MAX_TITLES) {
            if (de->d_name[0] == '.') continue;
            if (!is_hex8(de->d_name)) continue;

            SaveTitle *t = &out->titles[out->title_count];
            memset(t, 0, sizeof(*t));
            memcpy(t->title_id, "00050000", 8);
            for (int i = 0; i < 8; i++)
                t->title_id[8 + i] = (char)toupper((unsigned char)de->d_name[i]);
            t->title_id[16] = '\0';

            /* A title present on both devices would otherwise be listed
             * twice with the same id. */
            if (savetree_find(out, t->title_id)) continue;

            snprintf(t->root, sizeof(t->root), "%s/%s/user", root, de->d_name);
            if (!dir_exists(t->root)) continue;

            if (savetree_scan(t, NULL) != 0 || t->file_count == 0) {
                savetree_free_title(t);
                continue;
            }
            read_meta_longname(device, de->d_name, t->name, sizeof(t->name));
            out->title_count++;
        }
        closedir(d);
    }

    if (roots_opened == 0) {
        snprintf(out->last_error, sizeof(out->last_error),
                 "Cannot open %s", WIIU_SAVES_DIR);
        return;
    }
    if (out->title_count == 0)
        snprintf(out->last_error, sizeof(out->last_error), "No Wii U saves found");
}

/* ---- shared ---- */

bool natives_root_for(const char *title_id, char *out, size_t out_size) {
    if (vwiisaves_root_for(title_id, out, out_size)) return true;
    return wiiusaves_root_for(title_id, out, out_size);
}

int natives_backup(const SyncState *state, const char *title_id,
                   char *msg, size_t msg_size) {
    char src[SAVE_DIR_LEN];
    if (!natives_root_for(title_id, src, sizeof(src))) {
        snprintf(msg, msg_size, "Unknown native title id %s", title_id);
        return -1;
    }
    if (!dir_exists(src)) {
        snprintf(msg, msg_size, "No existing save to back up");
        return 0;   /* server-only title: nothing to protect */
    }

    char rel[TITLE_ID_LEN + 64];
    snprintf(rel, sizeof(rel), APP_DATA_SUBDIR "/backup/%s", title_id);
    char dst[SAVE_DIR_LEN];
    sdpath(state, rel, dst, sizeof(dst));

    if (savetree_copy_dir(src, dst) != 0) {
        snprintf(msg, msg_size, "Backup to %s FAILED", dst);
        return -1;
    }
    snprintf(msg, msg_size, "Backed up to %s", dst);
    return 0;
}
