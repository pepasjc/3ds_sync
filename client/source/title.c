#include "title.h"

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
        title_id_to_hex(ids[i], t->title_id_hex);
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
