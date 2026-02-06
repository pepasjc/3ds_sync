#include "bundle.h"
#include "sha256.h"

// Write a u16 little-endian
static void write_u16_le(u8 *buf, u16 val) {
    buf[0] = (u8)(val);
    buf[1] = (u8)(val >> 8);
}

// Write a u32 little-endian
static void write_u32_le(u8 *buf, u32 val) {
    buf[0] = (u8)(val);
    buf[1] = (u8)(val >> 8);
    buf[2] = (u8)(val >> 16);
    buf[3] = (u8)(val >> 24);
}

// Write a u64 big-endian
static void write_u64_be(u8 *buf, u64 val) {
    buf[0] = (u8)(val >> 56);
    buf[1] = (u8)(val >> 48);
    buf[2] = (u8)(val >> 40);
    buf[3] = (u8)(val >> 32);
    buf[4] = (u8)(val >> 24);
    buf[5] = (u8)(val >> 16);
    buf[6] = (u8)(val >> 8);
    buf[7] = (u8)(val);
}

// Read a u16 little-endian
static u16 read_u16_le(const u8 *buf) {
    return (u16)buf[0] | ((u16)buf[1] << 8);
}

// Read a u32 little-endian
static u32 read_u32_le(const u8 *buf) {
    return (u32)buf[0] | ((u32)buf[1] << 8) |
           ((u32)buf[2] << 16) | ((u32)buf[3] << 24);
}

// Read a u64 big-endian
static u64 read_u64_be(const u8 *buf) {
    return ((u64)buf[0] << 56) | ((u64)buf[1] << 48) |
           ((u64)buf[2] << 40) | ((u64)buf[3] << 32) |
           ((u64)buf[4] << 24) | ((u64)buf[5] << 16) |
           ((u64)buf[6] << 8)  | ((u64)buf[7]);
}

u8 *bundle_create(u64 title_id, u32 timestamp,
                  const ArchiveFile *files, int file_count,
                  u32 *out_size) {
    // Calculate total size
    u32 total_data = 0;
    u32 table_size = 0;
    for (int i = 0; i < file_count; i++) {
        u16 path_len = (u16)strlen(files[i].path);
        table_size += 2 + path_len + 4 + 32; // path_len + path + size + sha256
        total_data += files[i].size;
    }

    u32 header_size = 4 + 4 + 8 + 4 + 4 + 4; // magic + ver + tid + ts + count + total
    u32 bundle_size = header_size + table_size + total_data;

    u8 *buf = (u8 *)malloc(bundle_size);
    if (!buf) return NULL;

    u32 offset = 0;

    // Header
    memcpy(buf + offset, BUNDLE_MAGIC, 4); offset += 4;
    write_u32_le(buf + offset, BUNDLE_VERSION); offset += 4;
    write_u64_be(buf + offset, title_id); offset += 8;
    write_u32_le(buf + offset, timestamp); offset += 4;
    write_u32_le(buf + offset, (u32)file_count); offset += 4;
    write_u32_le(buf + offset, total_data); offset += 4;

    // File table
    for (int i = 0; i < file_count; i++) {
        u16 path_len = (u16)strlen(files[i].path);
        write_u16_le(buf + offset, path_len); offset += 2;
        memcpy(buf + offset, files[i].path, path_len); offset += path_len;
        write_u32_le(buf + offset, files[i].size); offset += 4;

        // SHA-256 of file data
        u8 hash[32];
        sha256(files[i].data, files[i].size, hash);
        memcpy(buf + offset, hash, 32); offset += 32;
    }

    // File data
    for (int i = 0; i < file_count; i++) {
        memcpy(buf + offset, files[i].data, files[i].size);
        offset += files[i].size;
    }

    *out_size = bundle_size;
    return buf;
}

int bundle_parse(const u8 *data, u32 data_size,
                 u64 *out_title_id, u32 *out_timestamp,
                 ArchiveFile *files, int max_files) {
    if (data_size < 28) return -1;

    u32 offset = 0;

    // Verify magic
    if (memcmp(data + offset, BUNDLE_MAGIC, 4) != 0) return -1;
    offset += 4;

    u32 version = read_u32_le(data + offset); offset += 4;
    if (version != BUNDLE_VERSION) return -1;

    *out_title_id = read_u64_be(data + offset); offset += 8;
    *out_timestamp = read_u32_le(data + offset); offset += 4;

    u32 file_count = read_u32_le(data + offset); offset += 4;
    /* u32 total_data = */ read_u32_le(data + offset); offset += 4;

    if ((int)file_count > max_files) return -1;

    // File table
    for (u32 i = 0; i < file_count; i++) {
        if (offset + 2 > data_size) return -1;
        u16 path_len = read_u16_le(data + offset); offset += 2;

        if (offset + path_len > data_size) return -1;
        if (path_len >= MAX_PATH_LEN) return -1;
        memcpy(files[i].path, data + offset, path_len);
        files[i].path[path_len] = '\0';
        offset += path_len;

        if (offset + 4 > data_size) return -1;
        files[i].size = read_u32_le(data + offset); offset += 4;

        // Skip hash (we verify on server side)
        if (offset + 32 > data_size) return -1;
        offset += 32;
    }

    // File data - point into the bundle buffer
    for (u32 i = 0; i < file_count; i++) {
        if (offset + files[i].size > data_size) return -1;
        files[i].data = (u8 *)(data + offset);
        offset += files[i].size;
    }

    return (int)file_count;
}

void bundle_compute_save_hash(const ArchiveFile *files, int file_count,
                              char *hex_out) {
    SHA256_CTX ctx;
    sha256_init(&ctx);

    for (int i = 0; i < file_count; i++) {
        sha256_update(&ctx, files[i].data, files[i].size);
    }

    u8 hash[32];
    sha256_final(&ctx, hash);

    for (int i = 0; i < 32; i++) {
        snprintf(hex_out + i * 2, 3, "%02x", hash[i]);
    }
    hex_out[64] = '\0';
}
