#include "title.h"
#include "network.h"

void title_id_to_hex(u64 title_id, char *out) {
    // Format as 16-char uppercase hex
    snprintf(out, 17, "%016llX", (unsigned long long)title_id);
}

// Check if a title has accessible save data by trying to open its archive
static bool title_has_save(u64 title_id, FS_MediaType media_type) {
    u32 path_data[3] = {media_type, (u32)(title_id & 0xFFFFFFFF), (u32)(title_id >> 32)};
    FS_Archive archive;

    Result res = FSUSER_OpenArchive(&archive, ARCHIVE_USER_SAVEDATA,
        (FS_Path){PATH_BINARY, sizeof(path_data), path_data});

    if (R_SUCCEEDED(res)) {
        FSUSER_CloseArchive(archive);
        return true;
    }
    return false;
}

// Scan a single media type for titles with save data
static int scan_media(FS_MediaType media_type, TitleInfo *titles, int offset, int max_titles) {
    u32 count = 0;
    int added = 0;

    Result res = AM_GetTitleCount(media_type, &count);
    if (R_FAILED(res) || count == 0)
        return 0;

    u64 *ids = (u64 *)malloc(count * sizeof(u64));
    if (!ids) return 0;

    u32 read = 0;
    res = AM_GetTitleList(&read, media_type, count, ids);
    if (R_FAILED(res)) {
        free(ids);
        return 0;
    }

    for (u32 i = 0; i < read && (offset + added) < max_titles; i++) {
        // Filter: only standard application titles (high word 0x00040000)
        u32 high = (u32)(ids[i] >> 32);
        if (high != 0x00040000 && high != 0x00040002) // games + demos
            continue;

        if (!title_has_save(ids[i], media_type))
            continue;

        TitleInfo *t = &titles[offset + added];
        t->title_id = ids[i];
        t->media_type = media_type;
        t->has_save_data = true;
        t->in_conflict = false;
        title_id_to_hex(ids[i], t->title_id_hex);

        // Get product code from AM
        t->product_code[0] = '\0';
        AM_GetTitleProductCode(media_type, ids[i], t->product_code);

        // Set initial name to product code (will be updated by server lookup)
        if (t->product_code[0]) {
            snprintf(t->name, sizeof(t->name), "%s", t->product_code);
        } else {
            snprintf(t->name, sizeof(t->name), "%.16s", t->title_id_hex);
        }
        added++;
    }

    free(ids);
    return added;
}

int titles_scan(TitleInfo *titles, int max_titles) {
    int total = 0;

    // Scan SD card (digital games)
    total += scan_media(MEDIATYPE_SD, titles, total, max_titles);

    // Scan game card
    total += scan_media(MEDIATYPE_GAME_CARD, titles, total, max_titles);

    return total;
}

// Minimal JSON parsing - find value for a key (string value)
static bool json_get_string(const char *json, const char *key, char *out, int out_size) {
    // Find "key":
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return false;

    pos += strlen(search);
    // Skip : and whitespace
    while (*pos == ':' || *pos == ' ' || *pos == '\t') pos++;
    if (*pos != '"') return false;
    pos++; // skip opening quote

    // Copy until closing quote
    int i = 0;
    while (*pos && *pos != '"' && i < out_size - 1) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return i > 0;
}

// Fetch game names from server for all titles
int titles_fetch_names(const AppConfig *config, TitleInfo *titles, int count) {
    if (count <= 0) return 0;

    // Build JSON request: {"codes": ["CTR-P-XXXX", ...]}
    // Estimate ~20 bytes per code
    int json_cap = count * 20 + 32;
    char *json = (char *)malloc(json_cap);
    if (!json) return 0;

    int pos = snprintf(json, json_cap, "{\"codes\":[");
    for (int i = 0; i < count; i++) {
        if (titles[i].product_code[0]) {
            if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
            pos += snprintf(json + pos, json_cap - pos, "\"%s\"", titles[i].product_code);
        }
    }
    pos += snprintf(json + pos, json_cap - pos, "]}");

    // POST to server
    u32 resp_size, status;
    u8 *resp = network_post_json(config, "/titles/names", json, &resp_size, &status);
    free(json);

    if (!resp || status != 200) {
        if (resp) free(resp);
        return 0;
    }

    // Null-terminate response
    char *resp_str = (char *)realloc(resp, resp_size + 1);
    if (!resp_str) { free(resp); return 0; }
    resp_str[resp_size] = '\0';

    // Parse response: {"names": {"CTR-P-XXXX": "Game Name", ...}}
    // Simple parsing: for each title with a product code, search for it in the response
    int updated = 0;
    for (int i = 0; i < count; i++) {
        if (!titles[i].product_code[0]) continue;

        char name[64];
        if (json_get_string(resp_str, titles[i].product_code, name, sizeof(name))) {
            snprintf(titles[i].name, sizeof(titles[i].name), "%s", name);
            updated++;
        }
    }

    free(resp_str);
    return updated;
}
