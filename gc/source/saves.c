/*
 * saves.c — GameCube memory-card save sync.
 *
 * Per-game GCI sync via libogc CARD_* (file-level, no client-side FS parse):
 *   - read:  CARD_GetStatusEx (64-byte card_direntry = GCI header) + CARD_Read
 *   - write: CARD_CreateEntry + CARD_Write + CARD_SetStatusEx (restore metadata)
 *
 * Wire: POST/GET /api/v1/saves/GC_<gamecode>/gc-card?format=gci  (body = GCI).
 */

#include "saves.h"
#include "http.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include <gccore.h>
#include <ogc/card.h>

static u8 g_workarea[CARD_WORKAREA_SIZE] __attribute__((aligned(32)));
static u8 g_gci[SAVES_GCI_MAX]           __attribute__((aligned(32)));

void saves_title_id_from_gamecode(const char *gamecode, char *out, size_t out_size) {
    char code[5] = {0};
    for (int i = 0; i < 4 && gamecode[i] && gamecode[i] != ' '; i++)
        code[i] = (char)toupper((unsigned char)gamecode[i]);
    snprintf(out, out_size, "GC_%s", code);
}

static const char *card_err_str(int rc) {
    switch (rc) {
        case CARD_ERROR_NOCARD:      return "no card in slot";
        case CARD_ERROR_WRONGDEVICE: return "wrong device";
        case CARD_ERROR_BROKEN:      return "card directory broken";
        case CARD_ERROR_IOERROR:     return "EXI I/O error";
        case CARD_ERROR_NOFILE:      return "file not found";
        case CARD_ERROR_NOENT:       return "no free block";
        case CARD_ERROR_INSSPACE:    return "not enough space";
        case CARD_ERROR_NOPERM:      return "no permission";
        default:                     return "error";
    }
}

/* ---- scan ---- */

void saves_scan_card(int port, GcSaveList *out) {
    if (!out) return;
    out->count = 0;
    out->port = port;
    out->last_error[0] = '\0';

    int rc = CARD_Mount(port, g_workarea, NULL);
    if (rc < 0) {
        snprintf(out->last_error, sizeof(out->last_error),
                 "Slot %c: %s (%d)", 'A' + port, card_err_str(rc), rc);
        return;
    }

    card_dir dirs[CARD_MAXFILES];
    s32 count = 0;
    rc = CARD_GetDirectory(port, dirs, &count, true);
    if (rc < 0) {
        CARD_Unmount(port);
        snprintf(out->last_error, sizeof(out->last_error),
                 "Slot %c: dir read failed (%d)", 'A' + port, rc);
        return;
    }

    for (int i = 0; i < count && out->count < SAVES_MAX_CARD; i++) {
        card_dir *d = &dirs[i];
        GcSave *s = &out->items[out->count];
        memset(s, 0, sizeof(*s));

        memcpy(s->gamecode, d->gamecode, 4); s->gamecode[4] = '\0';
        memcpy(s->company,  d->company,  2); s->company[2]  = '\0';
        memcpy(s->filename, d->filename, CARD_FILENAMELEN);
        s->filename[CARD_FILENAMELEN] = '\0';
        s->fileno = (int)d->fileno;
        s->size   = d->filelen;
        s->blocks = (int)(d->filelen / GC_BLOCK_SIZE);
        saves_title_id_from_gamecode(s->gamecode, s->title_id, sizeof(s->title_id));
        out->count++;
    }
    CARD_Unmount(port);

    if (out->count == 0)
        snprintf(out->last_error, sizeof(out->last_error),
                 "Slot %c: no saves on card", 'A' + port);
}

/* ---- upload ---- */

static int post_gci(const SyncState *state, const char *title_id,
                    const u8 *body, u32 len, char *msg, size_t msg_size) {
    char path[96];
    snprintf(path, sizeof(path), "/api/v1/saves/%s/gc-card?format=gci", title_id);

    HttpRequest req = {0};
    req.server_url        = state->server_url;
    req.api_key           = state->api_key;
    req.path              = path;
    req.method            = "POST";
    req.body              = body;
    req.body_len          = len;
    req.body_content_type = "application/octet-stream";

    static u8 resp[512];
    int status = 0;
    int n = http_get_buf(&req, resp, sizeof(resp), &status);
    if (n >= 0 && status == 200) {
        snprintf(msg, msg_size, "Uploaded %s (%u KB)", title_id, (unsigned)(len / 1024));
        return 0;
    }
    snprintf(msg, msg_size, "Upload %s failed (HTTP %d, n=%d)", title_id, status, n);
    return -1;
}

int saves_upload_card_game(const SyncState *state, int port, const GcSave *save,
                           char *msg, size_t msg_size) {
    if (!state || !save) return -1;

    int rc = CARD_Mount(port, g_workarea, NULL);
    if (rc < 0) { snprintf(msg, msg_size, "Mount slot %c failed (%d)", 'A' + port, rc); return -1; }

    card_file file;
    rc = CARD_Open(port, save->filename, &file);
    if (rc < 0) { CARD_Unmount(port); snprintf(msg, msg_size, "Open %s failed (%d)", save->filename, rc); return -1; }

    /* 64-byte directory entry (GCI header) at the front of the buffer. */
    rc = CARD_GetStatusEx(port, file.filenum, (card_direntry *)g_gci);
    if (rc < 0) { CARD_Close(&file); CARD_Unmount(port); snprintf(msg, msg_size, "Status failed (%d)", rc); return -1; }

    u32 datalen = (u32)file.len;
    if (64 + datalen > SAVES_GCI_MAX) {
        CARD_Close(&file); CARD_Unmount(port);
        snprintf(msg, msg_size, "Save too big (%u KB)", (unsigned)(datalen / 1024));
        return -1;
    }
    rc = CARD_Read(&file, g_gci + 64, datalen, 0);
    CARD_Close(&file);
    CARD_Unmount(port);
    if (rc < 0) { snprintf(msg, msg_size, "Read failed (%d)", rc); return -1; }

    return post_gci(state, save->title_id, g_gci, 64 + datalen, msg, msg_size);
}

/* ---- restore (download GCI -> card) ---- */

int saves_restore_card_game(const SyncState *state, int port, const char *title_id,
                            char *msg, size_t msg_size) {
    if (!state || !title_id || !title_id[0]) { snprintf(msg, msg_size, "No title id"); return -1; }

    char path[96];
    snprintf(path, sizeof(path), "/api/v1/saves/%s/gc-card?format=gci", title_id);

    HttpRequest req = {0};
    req.server_url = state->server_url;
    req.api_key    = state->api_key;
    req.path       = path;
    req.method     = "GET";

    int status = 0;
    int n = http_get_buf(&req, g_gci, SAVES_GCI_MAX, &status);
    if (n < 0 || status != 200) {
        snprintf(msg, msg_size, "Download %s failed (HTTP %d, n=%d)", title_id, status, n);
        return -1;
    }
    if (n < 64 + GC_BLOCK_SIZE) { snprintf(msg, msg_size, "GCI too small (%d B)", n); return -1; }

    card_direntry *de = (card_direntry *)g_gci;
    u32 datalen = (u32)n - 64;

    int rc = CARD_Mount(port, g_workarea, NULL);
    if (rc < 0) { snprintf(msg, msg_size, "Mount slot %c failed (%d)", 'A' + port, rc); return -1; }

    char fname[CARD_FILENAMELEN + 1];
    memcpy(fname, de->filename, CARD_FILENAMELEN);
    fname[CARD_FILENAMELEN] = '\0';

    /* Overwrite: a same-name file would otherwise fail create with EXIST. */
    CARD_Delete(port, fname);

    card_dir dir;
    memset(&dir, 0, sizeof(dir));
    memcpy(dir.gamecode, de->gamecode, 4);
    memcpy(dir.company,  de->company,  2);
    memcpy(dir.filename, de->filename, CARD_FILENAMELEN);
    dir.filelen     = datalen;
    dir.permissions = de->permission;
    dir.showall     = true;

    card_file file;
    rc = CARD_CreateEntry(port, &dir, &file);
    if (rc < 0) { CARD_Unmount(port); snprintf(msg, msg_size, "Create failed: %s (%d)", card_err_str(rc), rc); return -1; }

    rc = CARD_Write(&file, g_gci + 64, datalen, 0);
    if (rc < 0) { CARD_Close(&file); CARD_Unmount(port); snprintf(msg, msg_size, "Write failed (%d)", rc); return -1; }

    /* Restore icon/comment/time metadata (keeps the new block index + length). */
    CARD_SetStatusEx(port, file.filenum, de);
    CARD_Close(&file);
    CARD_Unmount(port);

    snprintf(msg, msg_size, "Restored %s to slot %c (%u KB)", title_id, 'A' + port,
             (unsigned)(datalen / 1024));
    return 0;
}

/* ---- server saves (JSON walk of /titles) ---- */

static const char *skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *find_key(const char *p, const char *end, const char *key) {
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0) return NULL;
    int depth = 0;
    while (p < end) {
        if (*p == '{' || *p == '[') { depth++; p++; continue; }
        if (*p == '}' || *p == ']') { if (depth == 0) return NULL; depth--; p++; continue; }
        if (*p == '"') {
            if (depth == 0 && (p + n) <= end && strncmp(p, needle, (size_t)n) == 0) {
                const char *q = skip_ws(p + n);
                if (q < end && *q == ':') return skip_ws(q + 1);
            }
            p++;
            while (p < end && *p != '"') { if (*p == '\\' && p + 1 < end) p++; p++; }
            if (p < end) p++;
            continue;
        }
        p++;
    }
    return NULL;
}

static bool extract_str(const char *p, const char *end, char *out, size_t out_size) {
    if (!p || p >= end || *p != '"') return false;
    p++;
    size_t len = 0;
    while (p < end && *p != '"' && len + 1 < out_size) {
        if (*p == '\\' && p + 1 < end) {
            char c = p[1];
            if (c == '"' || c == '\\' || c == '/') { out[len++] = c; p += 2; continue; }
            out[len++] = *p; p++;
            continue;
        }
        out[len++] = *p++;
    }
    out[len] = '\0';
    return true;
}

static bool extract_u32(const char *p, const char *end, uint32_t *out) {
    if (!p || p >= end) return false;
    p = skip_ws(p);
    char *endp = NULL;
    errno = 0;
    unsigned long long v = strtoull(p, &endp, 10);
    if (errno != 0 || endp == p) return false;
    *out = (uint32_t)v;
    return true;
}

static bool object_bounds(const char *p, const char *end, const char **end_out) {
    if (p >= end || *p != '{') return false;
    int depth = 0;
    while (p < end) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) { *end_out = p; return true; } }
        else if (*p == '"') { p++; while (p < end && *p != '"') { if (*p == '\\' && p + 1 < end) p++; p++; } }
        p++;
    }
    return false;
}

void saves_fetch_server(const SyncState *state,
                        char *scratch, uint32_t scratch_size,
                        ServerSaveList *out) {
    if (!out) return;
    out->count = 0;
    out->last_error[0] = '\0';

    HttpRequest req = {0};
    req.server_url = state->server_url;
    req.api_key    = state->api_key;
    req.path       = "/api/v1/titles?console_type=GC";
    req.method     = "GET";

    int status = 0;
    int n = http_get_buf(&req, (uint8_t *)scratch, scratch_size, &status);
    if (n < 0 || status != 200) {
        snprintf(out->last_error, sizeof(out->last_error),
                 "Server fetch failed (HTTP %d, n=%d)", status, n);
        return;
    }

    const char *body_end = scratch + n;
    /* Skip the root object's opening brace so find_key matches "titles" at
     * depth 0 (otherwise it descends to depth 1 and never matches). */
    const char *body = skip_ws(scratch);
    if (body < body_end && *body == '{') body++;
    const char *titles_v = find_key(body, body_end, "titles");
    if (!titles_v || *titles_v != '[') {
        snprintf(out->last_error, sizeof(out->last_error), "No 'titles' array");
        return;
    }

    const char *p = titles_v + 1;
    while (p < body_end && out->count < SAVES_MAX_SERVER) {
        p = skip_ws(p);
        if (p >= body_end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;

        const char *obj_end = NULL;
        if (!object_bounds(p, body_end, &obj_end)) break;

        ServerSave *s = &out->items[out->count];
        memset(s, 0, sizeof(*s));

        const char *v;
        v = find_key(p + 1, obj_end, "title_id");
        if (v) extract_str(v, obj_end, s->title_id, sizeof(s->title_id));
        v = find_key(p + 1, obj_end, "game_name");
        if (v) extract_str(v, obj_end, s->name, sizeof(s->name));
        if (!s->name[0]) {
            v = find_key(p + 1, obj_end, "name");
            if (v) extract_str(v, obj_end, s->name, sizeof(s->name));
        }
        v = find_key(p + 1, obj_end, "client_timestamp");
        if (v) extract_u32(v, obj_end, &s->timestamp);

        if (s->title_id[0]) {
            if (!s->name[0]) strncpy(s->name, s->title_id, sizeof(s->name) - 1);
            out->count++;
        }
        p = obj_end + 1;
    }

    if (out->count == 0 && out->last_error[0] == '\0')
        snprintf(out->last_error, sizeof(out->last_error), "No GC saves on server");
}

/* ---- VMC: full card images on SD ---- */

#define VMC_DIR SD_ROOT "/VMC"

static bool vmc_size_ok(uint32_t size) {
    return size >= 0x80000 && size <= 0x1000000 && (size % GC_BLOCK_SIZE) == 0;
}

static bool has_card_ext(const char *fname) {
    size_t n = strlen(fname);
    static const char *exts[] = { ".raw", ".gcp", ".mc", ".bin", ".mcd", NULL };
    for (int i = 0; exts[i]; i++) {
        size_t el = strlen(exts[i]);
        if (n > el && strcasecmp(fname + n - el, exts[i]) == 0) return true;
    }
    return false;
}

static void scan_vmc_dir(const char *dir, SaveVmcList *out) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && out->count < SAVES_MAX_VMC) {
        if (de->d_name[0] == '.') continue;
        if (!has_card_ext(de->d_name)) continue;
        char path[SAVE_DIR_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!vmc_size_ok((uint32_t)st.st_size)) continue;
        SaveVmc *v = &out->items[out->count];
        memset(v, 0, sizeof(*v));
        strncpy(v->path, path, sizeof(v->path) - 1);
        strncpy(v->filename, de->d_name, sizeof(v->filename) - 1);
        v->size = (uint32_t)st.st_size;
        out->count++;
    }
    closedir(d);
}

void saves_scan_vmc(SaveVmcList *out) {
    if (!out) return;
    out->count = 0;
    out->last_error[0] = '\0';
    scan_vmc_dir(VMC_DIR, out);
    scan_vmc_dir(SD_ROOT, out);
    if (out->count == 0)
        snprintf(out->last_error, sizeof(out->last_error),
                 "No card images in %s or sd:/", VMC_DIR);
}

int saves_upload_vmc(const SyncState *state, const SaveVmc *vmc, char *msg, size_t msg_size) {
    if (!state || !vmc) return -1;
    FILE *fp = fopen(vmc->path, "rb");
    if (!fp) { snprintf(msg, msg_size, "Open %s failed", vmc->filename); return -1; }
    uint8_t *buf = malloc(vmc->size);
    if (!buf) { fclose(fp); snprintf(msg, msg_size, "Out of memory (%u KB)", vmc->size / 1024); return -1; }
    size_t rd = fread(buf, 1, vmc->size, fp);
    fclose(fp);
    if (rd != vmc->size) { free(buf); snprintf(msg, msg_size, "Read %s short", vmc->filename); return -1; }

    HttpRequest req = {0};
    req.server_url        = state->server_url;
    req.api_key           = state->api_key;
    req.path              = "/api/v1/saves/gc-vmc/import";
    req.method            = "POST";
    req.body              = buf;
    req.body_len          = vmc->size;
    req.body_content_type = "application/octet-stream";

    static uint8_t resp[4096];
    int status = 0;
    int n = http_get_buf(&req, resp, sizeof(resp), &status);
    free(buf);
    if (n >= 0 && status == 200) {
        int games = 0;
        for (char *p = (char *)resp; (p = strstr(p, "title_id")) != NULL; p++) games++;
        snprintf(msg, msg_size, "Imported %d game(s) from %s", games, vmc->filename);
        return 0;
    }
    snprintf(msg, msg_size, "Import %s failed (HTTP %d, n=%d)", vmc->filename, status, n);
    return -1;
}

int saves_pull_all(const SyncState *state, const ServerSaveList *server,
                   char *msg, size_t msg_size) {
    if (!state || !server) return -1;
    mkdir(VMC_DIR, 0777);

    int ok = 0, fail = 0;
    for (int i = 0; i < server->count; i++) {
        const char *tid = server->items[i].title_id;
        char path[96];
        snprintf(path, sizeof(path), "/api/v1/saves/%s/gc-card?format=gci", tid);

        HttpRequest req = {0};
        req.server_url = state->server_url;
        req.api_key    = state->api_key;
        req.path       = path;
        req.method     = "GET";

        int status = 0;
        int n = http_get_buf(&req, g_gci, SAVES_GCI_MAX, &status);
        if (n < 0 || status != 200) { fail++; continue; }

        char out_path[SAVE_DIR_LEN];
        snprintf(out_path, sizeof(out_path), "%s/%s.gci", VMC_DIR, tid);
        FILE *fp = fopen(out_path, "wb");
        if (!fp) { fail++; continue; }
        size_t wr = fwrite(g_gci, 1, (size_t)n, fp);
        fclose(fp);
        if (wr == (size_t)n) ok++; else fail++;
    }
    snprintf(msg, msg_size, "Pulled %d GCI(s), %d failed", ok, fail);
    return ok;
}
