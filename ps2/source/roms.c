/*
 * roms.c — PS2 ROM catalog client.
 *
 * Same paginated JSON walker as the PSP / PS3 clients, with PS2-specific
 * routing rules:
 *
 *   PS2 (DVD) → mass:/DVD/<SLUS_213.71>.<title>.iso
 *   PS2 (CD)  → mass:/CD/<SLUS_213.71>.<title>.iso
 *   else      → mass:/3dssync/downloads/<filename>
 *
 * ``mass:`` can be USB or an internal ATA HDD exposed through the
 * PS2SDK BDM/FatFs stack.  The caller sets the detected root.
 *
 * Serial format: OPL expects ``SXXX_NNN.NN`` (8 chars + dot + 2 digits).
 * Server returns either that form or ``SXXX-NNNNN``; we normalise here.
 */

#include "roms.h"
#include "network.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static char g_storage_root[16] = STORAGE_DEFAULT_ROOT;
static char g_downloads_file[96] = DOWNLOADS_FILE;

static void storage_join(char *out, size_t out_size, const char *suffix) {
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "%s%s", g_storage_root, suffix ? suffix : "");
}

void roms_set_storage_root(const char *root) {
    if (!root || !root[0]) root = STORAGE_DEFAULT_ROOT;
    strncpy(g_storage_root, root, sizeof(g_storage_root) - 1);
    g_storage_root[sizeof(g_storage_root) - 1] = '\0';
    storage_join(g_downloads_file, sizeof(g_downloads_file),
                 STORAGE_DATA_SUBDIR "/downloads.dat");
}

const char *roms_storage_root(void) {
    return g_storage_root;
}

const char *roms_downloads_file(void) {
    return g_downloads_file;
}

void roms_storage_data_dir(char *out_path, size_t out_size) {
    storage_join(out_path, out_size, STORAGE_DATA_SUBDIR);
}

void roms_set_usb_root(const char *root) {
    roms_set_storage_root(root);
}

const char *roms_usb_root(void) {
    return roms_storage_root();
}

void roms_usb_data_dir(char *out_path, size_t out_size) {
    roms_storage_data_dir(out_path, out_size);
}

/* --- Tiny JSON helpers (lifted from the PSP client) --- */

static const char *skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *find_key(const char *p, const char *end, const char *key) {
    char needle[64];
    int  n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0) return NULL;

    int depth = 0;
    while (p < end) {
        if (*p == '{' || *p == '[') { depth++; p++; continue; }
        if (*p == '}' || *p == ']') {
            if (depth == 0) return NULL;
            depth--;
            p++;
            continue;
        }
        if (*p == '"') {
            if (depth == 0 && (p + n) <= end &&
                strncmp(p, needle, (size_t)n) == 0)
            {
                const char *q = p + n;
                q = skip_ws(q);
                if (q < end && *q == ':') return skip_ws(q + 1);
            }
            p++;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) p++;
                p++;
            }
            if (p < end) p++;
            continue;
        }
        p++;
    }
    return NULL;
}

static bool extract_str(const char *p, const char *end,
                        char *out, size_t out_size) {
    if (!p || p >= end || *p != '"') return false;
    p++;
    size_t len = 0;
    while (p < end && *p != '"' && len + 1 < out_size) {
        if (*p == '\\' && p + 1 < end) {
            char c = p[1];
            if (c == '"' || c == '\\' || c == '/') { out[len++] = c; p += 2; continue; }
            if (c == 'n') { out[len++] = '\n'; p += 2; continue; }
            if (c == 'r') { out[len++] = '\r'; p += 2; continue; }
            if (c == 't') { out[len++] = '\t'; p += 2; continue; }
            out[len++] = *p; p++;
            continue;
        }
        out[len++] = *p++;
    }
    out[len] = '\0';
    return true;
}

static bool extract_u64(const char *p, const char *end, uint64_t *out) {
    if (!p || p >= end) return false;
    p = skip_ws(p);
    if (p >= end) return false;
    char *endp = NULL;
    errno = 0;
    unsigned long long v = strtoull(p, &endp, 10);
    if (errno != 0 || endp == p) return false;
    *out = (uint64_t)v;
    return true;
}

static bool object_bounds(const char *p, const char *end, const char **end_out) {
    if (p >= end || *p != '{') return false;
    int depth = 0;
    while (p < end) {
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) { *end_out = p; return true; }
        } else if (*p == '"') {
            p++;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) p++;
                p++;
            }
        }
        p++;
    }
    return false;
}

/* --- PS2 disc-serial helpers --- */

static const char *PS2_SERIAL_PREFIXES[] = {
    "SLUS", "SCUS", "SLES", "SCES", "SLPS", "SLPM",
    "SCPS", "SCPM", "SLAJ", "SCAJ", "SCKA", "SLKA",
    "PAPX", "PBPX", "SLED",
    NULL
};

/* Try to pull a PS2 disc serial out of a string and return it in OPL
 * canonical form (``SXXX_NNN.NN``).  ``in`` can be a filename, the
 * catalog ``rom_id`` slug, or a free-form display name — anything that
 * might embed ``SLUS-21371`` / ``SLUS_213.71``. */
static bool extract_ps2_serial(const char *in, char *out, size_t out_size) {
    if (!in || !out || out_size < 12) return false;
    size_t len = strlen(in);
    for (size_t i = 0; i + 4 < len; i++) {
        for (int p = 0; PS2_SERIAL_PREFIXES[p]; p++) {
            if (strncasecmp(in + i, PS2_SERIAL_PREFIXES[p], 4) != 0) continue;
            const char *q = in + i + 4;
            if (*q == '-' || *q == '_' || *q == ' ' || *q == '.') q++;
            int digits = 0;
            char digit_buf[8];
            while (*q && digits < 5) {
                if (*q >= '0' && *q <= '9') {
                    digit_buf[digits++] = *q;
                    q++;
                } else if (*q == '.' || *q == '-' || *q == '_' || *q == ' ') {
                    q++;
                } else {
                    break;
                }
            }
            if (digits == 5) {
                snprintf(out, out_size, "%c%c%c%c_%c%c%c.%c%c",
                         (char)toupper((unsigned char)PS2_SERIAL_PREFIXES[p][0]),
                         (char)toupper((unsigned char)PS2_SERIAL_PREFIXES[p][1]),
                         (char)toupper((unsigned char)PS2_SERIAL_PREFIXES[p][2]),
                         (char)toupper((unsigned char)PS2_SERIAL_PREFIXES[p][3]),
                         digit_buf[0], digit_buf[1], digit_buf[2],
                         digit_buf[3], digit_buf[4]);
                return true;
            }
        }
    }
    return false;
}

/* Sanitise a display title for use inside an OPL filename.  OPL accepts
 * spaces but tools/users find ASCII-only safer.  We keep [A-Za-z0-9 ]
 * and collapse runs of spaces. */
static void sanitize_title(const char *in, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!in || !*in) { snprintf(out, out_size, "Game"); return; }

    /* OPL convention drops region/revision/translation tags from the
     * filename — so "Summoner (USA)" lands as "Summoner.iso", not
     * "Summoner USA.iso".  Cut off everything from the first ``(`` or
     * ``[``. Anything before that point is the actual game name. */
    size_t end = strlen(in);
    for (size_t i = 0; i < end; i++) {
        if (in[i] == '(' || in[i] == '[') { end = i; break; }
    }

    size_t j = 0;
    bool last_space = false;
    for (size_t i = 0; i < end && j + 1 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20) continue;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9'))
        {
            out[j++] = (char)c;
            last_space = false;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '_' || c == '-') {
            if (last_space || j == 0) continue;
            out[j++] = ' ';
            last_space = true;
            continue;
        }
        /* drop everything else (apostrophes, brackets, accents...) */
    }
    while (j > 0 && out[j-1] == ' ') j--;
    out[j] = '\0';
    if (j == 0) snprintf(out, out_size, "Game");
}

/* --- Catalog parse --- */

static bool parse_catalog_page(const char *scratch_buf, int n,
                               RomCatalog *catalog,
                               bool *has_more_out) {
    if (has_more_out) *has_more_out = false;

    const char *body_end = scratch_buf + n;
    const char *body = skip_ws(scratch_buf);
    if (body < body_end && *body == '{') body++;

    const char *more_v = find_key(body, body_end, "has_more");
    if (more_v && has_more_out) {
        const char *q = skip_ws(more_v);
        *has_more_out = (q < body_end && *q == 't');
    }

    const char *roms_v = find_key(body, body_end, "roms");
    if (!roms_v || *roms_v != '[') {
        snprintf(catalog->last_error, sizeof(catalog->last_error),
                 "Catalog response missing 'roms' array");
        return false;
    }

    const char *p = roms_v + 1;
    while (p < body_end && catalog->count < ROM_CATALOG_MAX) {
        p = skip_ws(p);
        if (p >= body_end) break;
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;

        const char *obj_end = NULL;
        if (!object_bounds(p, body_end, &obj_end)) break;

        RomEntry *e = &catalog->items[catalog->count];
        memset(e, 0, sizeof(*e));

        const char *v;
        v = find_key(p + 1, obj_end, "rom_id");
        if (v) extract_str(v, obj_end, e->rom_id, sizeof(e->rom_id));
        v = find_key(p + 1, obj_end, "filename");
        if (v) extract_str(v, obj_end, e->filename, sizeof(e->filename));
        v = find_key(p + 1, obj_end, "name");
        if (v) extract_str(v, obj_end, e->name, sizeof(e->name));
        v = find_key(p + 1, obj_end, "system");
        if (v) extract_str(v, obj_end, e->system, sizeof(e->system));
        v = find_key(p + 1, obj_end, "size");
        if (v) extract_u64(v, obj_end, &e->size);
        v = find_key(p + 1, obj_end, "extract_format");
        if (v) extract_str(v, obj_end, e->extract_format,
                           sizeof(e->extract_format));
        v = find_key(p + 1, obj_end, "is_bundle");
        if (v) {
            const char *q = skip_ws(v);
            e->is_bundle = (q < obj_end && *q == 't');
        }
        v = find_key(p + 1, obj_end, "file_count");
        if (v) {
            uint64_t fc = 0;
            if (extract_u64(v, obj_end, &fc) && fc < 64) {
                e->file_count = (int)fc;
            }
        }

        /* Fall back to filename when name missing. */
        if (!e->name[0] && e->filename[0]) {
            strncpy(e->name, e->filename, sizeof(e->name) - 1);
        }

        /* Derive normalised PS2 serial.  Try rom_id first (server
         * usually exposes it as the title_id when known), then name,
         * then filename. */
        if (!extract_ps2_serial(e->rom_id, e->serial, sizeof(e->serial)) &&
            !extract_ps2_serial(e->name,   e->serial, sizeof(e->serial)) &&
            !extract_ps2_serial(e->filename, e->serial, sizeof(e->serial)))
        {
            e->serial[0] = '\0';
        }

        /* CD vs DVD heuristic.  PS2 CD games are <700 MB and use the
         * SCUS / SLUS prefix range historically — but size alone is the
         * cleanest signal we have without inspecting the disc.  Server
         * support for an explicit ``is_cd`` flag can override later. */
        e->is_cd = (e->size > 0 && e->size <= 750ULL * 1024 * 1024);

        if (e->rom_id[0] && e->filename[0]) {
            catalog->count++;
        }
        p = obj_end + 1;
    }
    return true;
}

bool roms_fetch_catalog(const SyncState *state,
                        const char *system_code,
                        char *scratch_buf, uint32_t scratch_buf_size,
                        RomCatalog *catalog)
{
    if (!state || !catalog || !scratch_buf) return false;
    catalog->count = 0;
    catalog->last_error[0] = '\0';

    const int page_size = 200;   /* PS2 ISO entries are bigger; smaller pages save RAM */
    int offset = 0;
    int pages  = 0;

    while (catalog->count < ROM_CATALOG_MAX) {
        int status = 0;
        int n = network_fetch_rom_catalog(
            state,
            (system_code && system_code[0]) ? system_code : "PS2",
            offset, page_size,
            scratch_buf, scratch_buf_size, &status);
        if (n <= 0 || status != 200) {
            if (catalog->count == 0) {
                if (n == -100) {
                    snprintf(catalog->last_error, sizeof(catalog->last_error),
                             "Network not ready (ip=%s)", state->ip);
                } else {
                    snprintf(catalog->last_error, sizeof(catalog->last_error),
                             "Catalog fetch failed (HTTP %d, n=%d)", status, n);
                }
                return false;
            }
            break;
        }

        bool has_more = false;
        int  before   = catalog->count;
        if (!parse_catalog_page(scratch_buf, n, catalog, &has_more)) {
            return false;
        }
        int parsed = catalog->count - before;
        pages++;
        if (!has_more || parsed == 0) break;
        offset += page_size;
        if (pages >= (ROM_CATALOG_MAX / page_size) + 2) break;
    }
    return true;
}

const char *roms_preferred_extract_format(const RomEntry *rom) {
    if (!rom) return "";
    /* PS2 CHDs come back with extract_format="ps2" — pick "iso" so OPL
     * gets a mountable disc image.  CSO support for PS2 is patchy
     * across loaders so we don't request it. */
    if (strcasecmp(rom->extract_format, "ps2") == 0) return "iso";
    return rom->extract_format;
}

void roms_mkdir_p(const char *path) {
    if (!path || !*path) return;

    char buf[256];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = buf;
    char *colon = strchr(buf, ':');
    if (colon) {
        p = colon + 1;
        if (*p == '/') p++;
    } else if (*p == '/') {
        p++;
    }

    for (; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (buf[0] != '\0') mkdir(buf, 0777);
            *p = '/';
        }
    }
    mkdir(buf, 0777);
}

void roms_ensure_target_dirs(void) {
    char path[128];
    storage_join(path, sizeof(path), "/DVD");
    roms_mkdir_p(path);
    storage_join(path, sizeof(path), "/CD");
    roms_mkdir_p(path);
    storage_join(path, sizeof(path), STORAGE_DATA_SUBDIR "/downloads");
    roms_mkdir_p(path);
}

bool roms_resolve_target_path(const RomEntry *rom,
                              char *out_path, size_t out_size)
{
    if (!rom || !out_path || out_size < 32) return false;

    /* Non-PS2 entries shouldn't appear in the PS2 catalog, but keep a
     * safe fallback in case the user points at a different system. */
    if (strcasecmp(rom->system, "PS2") != 0) {
        char fallback[128];
        storage_join(fallback, sizeof(fallback), STORAGE_DATA_SUBDIR "/downloads");
        int n = snprintf(out_path, out_size,
                         "%s/%s", fallback, rom->filename);
        return n > 0 && (size_t)n < out_size;
    }

    /* Need a serial.  Without one OPL can't show per-game art so we
     * fall back to a synthesised SLUS_000.00 — better than dropping
     * the file into nowhere. */
    char serial[16];
    if (rom->serial[0]) {
        strncpy(serial, rom->serial, sizeof(serial) - 1);
        serial[sizeof(serial) - 1] = '\0';
    } else {
        snprintf(serial, sizeof(serial), "SLUS_000.00");
    }

    char title[80];
    sanitize_title(rom->name[0] ? rom->name : rom->filename,
                   title, sizeof(title));

    /* OPL truncates display name at ~32 chars in the GUI.  Trim hard so
     * the full path stays under 256 bytes (mass: + DVD/ + serial + . +
     * title + .iso ≈ 60 chars guaranteed budget). */
    if (strlen(title) > 32) title[32] = '\0';

    char root[64];
    storage_join(root, sizeof(root), rom->is_cd ? "/CD" : "/DVD");
    int n = snprintf(out_path, out_size,
                     "%s/%s.%s.iso", root, serial, title);
    return n > 0 && (size_t)n < out_size;
}

/* ---- Local ISO scan ----
 *
 * OPL-format filenames look like:
 *
 *     SLUS_200.74.Summoner.iso
 *     SCUS_973.99.Twisted Metal Black.iso
 *
 * Parse: 4 letters + '_' + 3 digits + '.' + 2 digits + '.' + title + '.iso'.
 * We accept any case for the prefix but normalise to upper.
 */
static bool parse_opl_filename(const char *fname, LocalRom *out) {
    if (!fname || !out) return false;
    size_t n = strlen(fname);
    if (n < 16) return false;

    /* Must end with ".iso" (case-insensitive). */
    if (strcasecmp(fname + n - 4, ".iso") != 0) return false;

    /* Need at least 11 chars of OPL header before the title. */
    if (!isalpha((unsigned char)fname[0]) ||
        !isalpha((unsigned char)fname[1]) ||
        !isalpha((unsigned char)fname[2]) ||
        !isalpha((unsigned char)fname[3]))
    {
        return false;
    }
    if (fname[4] != '_') return false;
    if (!isdigit((unsigned char)fname[5]) ||
        !isdigit((unsigned char)fname[6]) ||
        !isdigit((unsigned char)fname[7]))
    {
        return false;
    }
    if (fname[8] != '.') return false;
    if (!isdigit((unsigned char)fname[9]) ||
        !isdigit((unsigned char)fname[10]))
    {
        return false;
    }
    if (fname[11] != '.') return false;

    snprintf(out->serial, sizeof(out->serial),
             "%c%c%c%c_%c%c%c.%c%c",
             (char)toupper((unsigned char)fname[0]),
             (char)toupper((unsigned char)fname[1]),
             (char)toupper((unsigned char)fname[2]),
             (char)toupper((unsigned char)fname[3]),
             fname[5], fname[6], fname[7],
             fname[9], fname[10]);

    /* Title: between header and ".iso". */
    size_t title_len = n - 12 - 4;
    if (title_len >= sizeof(out->name)) title_len = sizeof(out->name) - 1;
    memcpy(out->name, fname + 12, title_len);
    out->name[title_len] = '\0';

    strncpy(out->filename, fname, sizeof(out->filename) - 1);
    out->filename[sizeof(out->filename) - 1] = '\0';
    return true;
}

static int local_scan_dir(const char *dir_path, bool is_cd,
                          LocalRomList *out)
{
    DIR *d = opendir(dir_path);
    if (!d) return 0;

    int added = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (out->count >= LOCAL_ROMS_MAX) break;
        if (de->d_name[0] == '.') continue;

        LocalRom *e = &out->items[out->count];
        memset(e, 0, sizeof(*e));
        if (!parse_opl_filename(de->d_name, e)) continue;

        e->is_cd = is_cd;
        snprintf(e->path, sizeof(e->path), "%s/%s", dir_path, de->d_name);

        struct stat st;
        if (stat(e->path, &st) == 0) {
            e->size = (uint64_t)st.st_size;
        }

        out->count++;
        added++;
    }
    closedir(d);
    return added;
}

void roms_scan_local(LocalRomList *out) {
    if (!out) return;
    out->count = 0;
    out->last_error[0] = '\0';

    char dvd_path[64], cd_path[64];
    storage_join(dvd_path, sizeof(dvd_path), "/DVD");
    storage_join(cd_path,  sizeof(cd_path),  "/CD");

    int dvd_count = local_scan_dir(dvd_path, false, out);
    int cd_count  = local_scan_dir(cd_path,  true,  out);

    if (dvd_count == 0 && cd_count == 0 && out->count == 0) {
        snprintf(out->last_error, sizeof(out->last_error),
                 "No ISOs found in %s or %s", dvd_path, cd_path);
    }
}
