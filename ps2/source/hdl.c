/*
 * hdl.c - APA/HDLoader installs for PS2 fat internal HDDs.
 *
 * We create APA partitions with ps2hdd's normal PFS type, add any needed
 * sub-partitions, then patch the APA type to 0x1337 after the ISO data and
 * OPL/HDLoader metadata have landed.  That keeps creation compatible with
 * stock ps2hdd.irx builds that do not expose "HDL" as a creatable type.
 */

#include "hdl.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <hdd-ioctl.h>
#include <io_common.h>

#ifndef APA_MAGIC
#define APA_MAGIC 0x00415041
#endif
#ifndef APA_TYPE_HDL
#define APA_TYPE_HDL 0x1337
#endif
#ifndef APA_FLAG_SUB
#define APA_FLAG_SUB 0x0001
#endif

#define SECTOR_SIZE 512U
#define MIB_BYTES   (1024ULL * 1024ULL)

#define HDL_MAIN_DATA_OFFSET_SECTORS 0x2000U  /* 4 MiB */
#define HDL_SUB_DATA_OFFSET_SECTORS  0x0800U  /* 1 MiB */
#define HDL_INFO_OFFSET_SECTORS      0x0808U  /* (0x100000 + 4096) / 512 */
#define HDL_DISC_CD                  0x12U
#define HDL_DISC_DVD                 0x14U
#define HDL_MAGIC                    0xDEADFEEDU
#define HDL_RAW_MAX_SECTORS          64U

typedef struct {
    uint8_t  unused;
    uint8_t  sec;
    uint8_t  min;
    uint8_t  hour;
    uint8_t  day;
    uint8_t  month;
    uint16_t year;
} HdlPs2Time;

typedef struct {
    uint32_t start;
    uint32_t length;
} HdlApaSub;

typedef struct {
    uint32_t checksum;
    uint32_t magic;
    uint32_t next;
    uint32_t prev;
    char     id[APA_IDMAX];
    char     rpwd[APA_PASSMAX];
    char     fpwd[APA_PASSMAX];
    uint32_t start;
    uint32_t length;
    uint16_t type;
    uint16_t flags;
    uint32_t nsub;
    HdlPs2Time created;
    uint32_t main;
    uint32_t number;
    uint32_t modver;
    uint32_t padding1[7];
    char     padding2[128];
    struct {
        char     magic[32];
        uint32_t version;
        uint32_t nsector;
        HdlPs2Time created;
        uint32_t osdStart;
        uint32_t osdSize;
        char     padding3[200];
    } mbr;
    HdlApaSub subs[APA_MAXSUB];
} HdlApaHeader;

typedef struct {
    uint32_t part_offset;  /* 2048-byte sector offset in the ISO stream */
    uint32_t data_start;   /* absolute 512-byte HDD sector */
    uint32_t part_size;    /* bytes in this mapped region */
} HdlPartSpec;

typedef struct {
    uint32_t checksum;
    uint32_t magic;
    char     gamename[160];
    uint8_t  hdl_compat_flags;
    uint8_t  ops2l_compat_flags;
    uint8_t  dma_type;
    uint8_t  dma_mode;
    char     startup[60];
    uint32_t layer1_start;
    uint32_t discType;
    int32_t  num_partitions;
    HdlPartSpec part_specs[HDL_MAX_PARTS];
} HdlInfoHeader;

typedef char HdlApaHeader_size_check[(sizeof(HdlApaHeader) == 1024) ? 1 : -1];
typedef char HdlInfoHeader_size_check[(sizeof(HdlInfoHeader) == 1024) ? 1 : -1];

typedef struct {
    uint32_t lba;
    uint32_t size;
} HdlReadArg;

typedef struct {
    uint32_t lba;
    uint32_t size;
    uint8_t  data[HDL_RAW_MAX_SECTORS * SECTOR_SIZE];
} HdlWriteArg;

typedef struct {
    int size_mb;
    const char *size_str;
    uint64_t capacity;
} HdlPlanPart;

static HdlApaHeader g_apa_buf __attribute__((aligned(64)));
static HdlInfoHeader g_info_buf __attribute__((aligned(64)));
static HdlReadArg g_read_arg __attribute__((aligned(64)));
static HdlWriteArg g_write_arg __attribute__((aligned(64)));

static const int HDL_SIZE_MB[] = {128, 256, 512, 1024, 2048, 4096};
static const char *HDL_SIZE_STR[] = {"128M", "256M", "512M", "1G", "2G", "4G"};

static int raw_read_sectors(uint32_t lba, uint32_t count, void *buf) {
    g_read_arg.lba  = lba;
    g_read_arg.size = count;
    return fileXioDevctl("hdd0:", HDIOC_READSECTOR,
                         &g_read_arg, sizeof(g_read_arg),
                         buf, count * SECTOR_SIZE);
}

static int raw_write_sectors(uint32_t lba, uint32_t count, const void *buf) {
    const uint8_t *p = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t take = count > HDL_RAW_MAX_SECTORS
                      ? HDL_RAW_MAX_SECTORS
                      : count;
        g_write_arg.lba  = lba;
        g_write_arg.size = take;
        memcpy(g_write_arg.data, p, take * SECTOR_SIZE);
        int rc = fileXioDevctl("hdd0:", HDIOC_WRITESECTOR,
                               &g_write_arg,
                               sizeof(hddAtaTransfer_t) + take * SECTOR_SIZE,
                               NULL, 0);
        if (rc != 0) return rc;
        lba   += take;
        p     += take * SECTOR_SIZE;
        count -= take;
    }
    return 0;
}

static uint32_t apa_checksum(const HdlApaHeader *header) {
    const uint32_t *p = (const uint32_t *)header;
    uint32_t sum = 0;
    for (int i = 1; i < 256; i++) sum += p[i];
    return sum;
}

static int apa_write_header(HdlApaHeader *header) {
    header->checksum = 0;
    header->checksum = apa_checksum(header);
    return raw_write_sectors(header->start, sizeof(HdlApaHeader) / SECTOR_SIZE,
                             header);
}

static bool partition_id_matches(const char *header_id, const char *id) {
    char padded[APA_IDMAX];
    memset(padded, 0, sizeof(padded));
    strncpy(padded, id, sizeof(padded) - 1);
    return memcmp(header_id, padded, APA_IDMAX) == 0;
}

static bool find_partition(const char *id, HdlApaHeader *out) {
    uint32_t lba = 0;
    for (int guard = 0; guard < 4096; guard++) {
        if (raw_read_sectors(lba, sizeof(HdlApaHeader) / SECTOR_SIZE,
                             &g_apa_buf) != 0) {
            return false;
        }
        if (g_apa_buf.magic != APA_MAGIC) return false;

        if (!(g_apa_buf.flags & APA_FLAG_SUB) &&
            partition_id_matches(g_apa_buf.id, id))
        {
            if (out) memcpy(out, &g_apa_buf, sizeof(*out));
            return true;
        }

        if (g_apa_buf.next == 0 || g_apa_buf.next == lba) break;
        lba = g_apa_buf.next;
    }
    return false;
}

static void serial_to_hdl(const char *serial, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "SLUS-00000");
    if (!serial || strlen(serial) < 4) return;

    char prefix[5];
    for (int i = 0; i < 4; i++) {
        if (!isalpha((unsigned char)serial[i])) return;
        prefix[i] = (char)toupper((unsigned char)serial[i]);
    }
    prefix[4] = '\0';

    char digits[6];
    int d = 0;
    for (const char *p = serial + 4; *p && d < 5; p++) {
        if (isdigit((unsigned char)*p)) digits[d++] = *p;
    }
    if (d != 5) return;
    digits[5] = '\0';
    snprintf(out, out_size, "%s-%s", prefix, digits);
}

static void title_to_partition_slug(const char *title, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t j = 0;
    bool last_sep = false;
    if (!title || !title[0]) title = "GAME";

    for (const char *p = title; *p && j + 1 < out_size; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '(' || c == '[') break;
        if (isalnum(c)) {
            out[j++] = (char)toupper(c);
            last_sep = false;
        } else if (!last_sep && j > 0) {
            out[j++] = '_';
            last_sep = true;
        }
    }
    while (j > 0 && out[j - 1] == '_') j--;
    out[j] = '\0';
    if (j == 0) snprintf(out, out_size, "GAME");
}

bool hdl_make_partition_name(const char *serial,
                             const char *title,
                             char *out, size_t out_size)
{
    if (!out || out_size < HDL_PARTITION_NAME_LEN) return false;

    char hdl_serial[16];
    char slug[32];
    serial_to_hdl(serial, hdl_serial, sizeof(hdl_serial));

    /* APA IDs are 32 bytes. Keep a trailing NUL for fileXio path strings. */
    size_t prefix_len = strlen("PP.") + strlen(hdl_serial) + strlen("..");
    size_t slug_cap = HDL_PARTITION_NAME_LEN - prefix_len;
    if (slug_cap > sizeof(slug)) slug_cap = sizeof(slug);
    title_to_partition_slug(title, slug, slug_cap);

    snprintf(out, out_size, "PP.%s..%s", hdl_serial, slug);
    out[HDL_PARTITION_NAME_LEN - 1] = '\0';
    return true;
}

bool hdl_resolve_target_path_from_rom(const RomEntry *rom,
                                      char *out_path, size_t out_size)
{
    if (!rom || !out_path || out_size < 40) return false;
    char part[HDL_PARTITION_NAME_LEN];
    if (!hdl_make_partition_name(rom->serial,
                                 rom->name[0] ? rom->name : rom->filename,
                                 part, sizeof(part))) {
        return false;
    }
    int n = snprintf(out_path, out_size, "hdd0:%s", part);
    return n > 0 && (size_t)n < out_size;
}

bool hdl_resolve_target_path_from_download(const DownloadEntry *entry,
                                           char *out_path, size_t out_size)
{
    if (!entry || !out_path || out_size < 40) return false;
    char part[HDL_PARTITION_NAME_LEN];
    if (!hdl_make_partition_name(entry->serial,
                                 entry->name[0] ? entry->name : entry->filename,
                                 part, sizeof(part))) {
        return false;
    }
    int n = snprintf(out_path, out_size, "hdd0:%s", part);
    return n > 0 && (size_t)n < out_size;
}

static const char *path_partition_id(const char *path) {
    if (!path) return "";
    const char *colon = strchr(path, ':');
    return colon ? colon + 1 : path;
}

static uint64_t partition_usable_bytes(uint32_t length_sectors,
                                       uint32_t header_sectors)
{
    if (length_sectors <= header_sectors) return 0;
    return (uint64_t)(length_sectors - header_sectors) * SECTOR_SIZE;
}

static int hdl_max_size_index(void) {
    int max_mb = 4096;
    int max_sectors = fileXioDevctl("hdd0:", HDIOC_MAXSECTOR,
                                    NULL, 0, NULL, 0);
    if (max_sectors > 0) {
        int by_driver = max_sectors / 2048;
        if (by_driver > 0 && by_driver < max_mb) max_mb = by_driver;
    }

    int max_index = 0;
    int count = (int)(sizeof(HDL_SIZE_MB) / sizeof(HDL_SIZE_MB[0]));
    for (int i = 0; i < count; i++) {
        if (HDL_SIZE_MB[i] <= max_mb) max_index = i;
    }
    return max_index;
}

static int plan_partitions(uint64_t image_size,
                           HdlPlanPart *plan,
                           int *plan_count_out)
{
    if (!plan || !plan_count_out || image_size == 0) return -EINVAL;
    int max_index = hdl_max_size_index();
    uint64_t remaining = image_size;
    int count = 0;

    while (remaining > 0 && count < HDL_MAX_PARTS) {
        uint64_t overhead = (count == 0) ? 4ULL * MIB_BYTES : 1ULL * MIB_BYTES;
        int chosen = -1;
        for (int i = 0; i <= max_index; i++) {
            uint64_t cap = (uint64_t)HDL_SIZE_MB[i] * MIB_BYTES;
            if (cap <= overhead) continue;
            cap -= overhead;
            if (cap >= remaining) {
                chosen = i;
                break;
            }
        }
        if (chosen < 0) chosen = max_index;

        plan[count].size_mb = HDL_SIZE_MB[chosen];
        plan[count].size_str = HDL_SIZE_STR[chosen];
        plan[count].capacity =
            (uint64_t)HDL_SIZE_MB[chosen] * MIB_BYTES - overhead;

        uint64_t take = remaining < plan[count].capacity
                      ? remaining
                      : plan[count].capacity;
        remaining -= take;
        count++;
    }

    if (remaining > 0) return -ENOSPC;
    *plan_count_out = count;
    return 0;
}

static int create_partition_set(const char *partition,
                                const HdlPlanPart *plan,
                                int plan_count)
{
    char path[48];
    snprintf(path, sizeof(path), "hdd0:%s", partition);
    fileXioRemove(path);

    char open_path[96];
    snprintf(open_path, sizeof(open_path), "hdd0:%s,,,%s,PFS",
             partition, plan[0].size_str);

    int fd = fileXioOpen(open_path, FIO_O_RDWR | FIO_O_CREAT);
    if (fd < 0) return fd;

    for (int i = 1; i < plan_count; i++) {
        int rc = fileXioIoctl2(fd, HIOCADDSUB,
                               (void *)plan[i].size_str,
                               strlen(plan[i].size_str) + 1,
                               NULL, 0);
        if (rc < 0) {
            fileXioClose(fd);
            fileXioRemove(path);
            return rc;
        }
    }
    fileXioClose(fd);
    return 0;
}

static int build_region_map(HdlInstall *ctx) {
    HdlApaHeader header;
    if (!find_partition(ctx->partition, &header)) return -ENOENT;

    int nsub = (header.nsub > APA_MAXSUB) ? APA_MAXSUB : (int)header.nsub;
    ctx->region_count = nsub + 1;
    uint64_t image_offset = 0;

    ctx->regions[0].start_lba =
        header.start + HDL_MAIN_DATA_OFFSET_SECTORS;
    ctx->regions[0].image_offset = image_offset;
    ctx->regions[0].capacity =
        partition_usable_bytes(header.length, HDL_MAIN_DATA_OFFSET_SECTORS);
    image_offset += ctx->regions[0].capacity;

    for (int i = 0; i < nsub; i++) {
        ctx->regions[i + 1].start_lba =
            header.subs[i].start + HDL_SUB_DATA_OFFSET_SECTORS;
        ctx->regions[i + 1].image_offset = image_offset;
        ctx->regions[i + 1].capacity =
            partition_usable_bytes(header.subs[i].length,
                                   HDL_SUB_DATA_OFFSET_SECTORS);
        image_offset += ctx->regions[i + 1].capacity;
    }

    return 0;
}

static HdlRegion *find_write_region(HdlInstall *ctx, uint64_t offset) {
    for (int i = 0; i < ctx->region_count; i++) {
        HdlRegion *r = &ctx->regions[i];
        if (offset >= r->image_offset &&
            offset < r->image_offset + r->capacity)
        {
            return r;
        }
    }
    return NULL;
}

static int write_aligned(HdlInstall *ctx, const uint8_t *data, uint32_t len) {
    if ((ctx->offset % SECTOR_SIZE) != 0 || (len % SECTOR_SIZE) != 0) {
        return -EINVAL;
    }

    while (len > 0) {
        HdlRegion *r = find_write_region(ctx, ctx->offset);
        if (!r) return -ENOSPC;

        uint64_t region_off = ctx->offset - r->image_offset;
        uint64_t left_in_region = r->capacity - region_off;
        uint32_t can = len;
        if ((uint64_t)can > left_in_region) can = (uint32_t)left_in_region;
        can &= ~(SECTOR_SIZE - 1);
        if (can == 0) return -ENOSPC;

        uint32_t sectors = can / SECTOR_SIZE;
        uint32_t lba = r->start_lba + (uint32_t)(region_off / SECTOR_SIZE);
        int rc = raw_write_sectors(lba, sectors, data);
        if (rc != 0) return rc;

        ctx->offset += can;
        data += can;
        len -= can;
    }
    return 0;
}

int hdl_install_write(const void *data, uint32_t len, void *user) {
    HdlInstall *ctx = (HdlInstall *)user;
    if (!ctx || !ctx->active || (!data && len > 0)) return -EINVAL;

    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        if (ctx->pending_len > 0) {
            uint32_t need = SECTOR_SIZE - ctx->pending_len;
            uint32_t take = len < need ? len : need;
            memcpy(ctx->pending + ctx->pending_len, p, take);
            ctx->pending_len += take;
            p += take;
            len -= take;
            if (ctx->pending_len == SECTOR_SIZE) {
                int rc = write_aligned(ctx, ctx->pending, SECTOR_SIZE);
                if (rc != 0) return rc;
                ctx->pending_len = 0;
            }
            continue;
        }

        if (len >= SECTOR_SIZE) {
            uint32_t aligned = len & ~(SECTOR_SIZE - 1);
            int rc = write_aligned(ctx, p, aligned);
            if (rc != 0) return rc;
            p += aligned;
            len -= aligned;
        } else {
            memcpy(ctx->pending, p, len);
            ctx->pending_len = len;
            len = 0;
        }
    }
    return 0;
}

static int write_hdl_info(HdlInstall *ctx) {
    memset(&g_info_buf, 0, sizeof(g_info_buf));
    g_info_buf.checksum = HDL_MAGIC;
    strncpy(g_info_buf.gamename, ctx->game_name,
            sizeof(g_info_buf.gamename) - 1);
    strncpy(g_info_buf.startup, ctx->startup,
            sizeof(g_info_buf.startup) - 1);
    g_info_buf.discType = ctx->is_cd ? HDL_DISC_CD : HDL_DISC_DVD;
    g_info_buf.num_partitions = ctx->region_count;

    uint64_t remaining = ctx->image_size;
    for (int i = 0; i < ctx->region_count && remaining > 0; i++) {
        HdlRegion *r = &ctx->regions[i];
        uint64_t used = remaining < r->capacity ? remaining : r->capacity;
        uint64_t spec_size = (used + 2047ULL) & ~2047ULL;
        if (spec_size > r->capacity) spec_size = r->capacity;
        if (spec_size > 0xFFFFFFFFULL) return -EFBIG;

        g_info_buf.part_specs[i].part_offset =
            (uint32_t)(r->image_offset / 2048ULL);
        g_info_buf.part_specs[i].data_start = r->start_lba;
        g_info_buf.part_specs[i].part_size = (uint32_t)spec_size;
        remaining -= used;
    }

    HdlApaHeader main_header;
    if (!find_partition(ctx->partition, &main_header)) return -ENOENT;
    return raw_write_sectors(main_header.start + HDL_INFO_OFFSET_SECTORS,
                             sizeof(g_info_buf) / SECTOR_SIZE,
                             &g_info_buf);
}

static int patch_apa_type(HdlInstall *ctx) {
    HdlApaHeader main_header;
    if (!find_partition(ctx->partition, &main_header)) return -ENOENT;

    uint32_t sub_starts[APA_MAXSUB];
    int nsub = (main_header.nsub > APA_MAXSUB)
             ? APA_MAXSUB
             : (int)main_header.nsub;
    for (int i = 0; i < nsub; i++) sub_starts[i] = main_header.subs[i].start;

    int rc = 0;
    for (int i = 0; i < nsub; i++) {
        if (raw_read_sectors(sub_starts[i],
                             sizeof(HdlApaHeader) / SECTOR_SIZE,
                             &g_apa_buf) != 0) {
            return -EIO;
        }
        if (g_apa_buf.magic != APA_MAGIC) return -EIO;
        g_apa_buf.type = APA_TYPE_HDL;
        rc = apa_write_header(&g_apa_buf);
        if (rc != 0) return rc;
    }

    /* Main header is patched last so an interrupted install is not listed
     * as a valid HDL game by OPL. */
    main_header.type = APA_TYPE_HDL;
    rc = apa_write_header(&main_header);
    if (rc != 0) return rc;
    return 0;
}

int hdl_install_begin(HdlInstall *ctx,
                      const DownloadEntry *entry,
                      uint64_t content_length)
{
    if (!ctx || !entry) return -EINVAL;
    memset(ctx, 0, sizeof(*ctx));

    ctx->image_size = content_length > 0 ? content_length : entry->total;
    if (ctx->image_size == 0) return -EINVAL;

    strncpy(ctx->target_path, entry->target_path, sizeof(ctx->target_path) - 1);
    if (ctx->target_path[0] == '\0' ||
        strncasecmp(ctx->target_path, "hdd0:", 5) != 0) {
        if (!hdl_resolve_target_path_from_download(entry,
                                                   ctx->target_path,
                                                   sizeof(ctx->target_path))) {
            return -EINVAL;
        }
    }
    strncpy(ctx->partition, path_partition_id(ctx->target_path),
            sizeof(ctx->partition) - 1);
    strncpy(ctx->game_name,
            entry->name[0] ? entry->name : entry->filename,
            sizeof(ctx->game_name) - 1);
    strncpy(ctx->startup,
            entry->serial[0] ? entry->serial : "SLUS_000.00",
            sizeof(ctx->startup) - 1);
    ctx->is_cd = entry->is_cd;

    HdlPlanPart plan[HDL_MAX_PARTS];
    int plan_count = 0;
    int rc = plan_partitions(ctx->image_size, plan, &plan_count);
    if (rc != 0) return rc;

    rc = create_partition_set(ctx->partition, plan, plan_count);
    if (rc != 0) return rc;

    rc = build_region_map(ctx);
    if (rc != 0) {
        fileXioRemove(ctx->target_path);
        return rc;
    }

    ctx->active = true;
    return 0;
}

int hdl_install_finish(HdlInstall *ctx, bool success) {
    if (!ctx || !ctx->active) return 0;

    int rc = 0;
    if (success) {
        if (ctx->pending_len > 0) {
            memset(ctx->pending + ctx->pending_len, 0,
                   SECTOR_SIZE - ctx->pending_len);
            rc = write_aligned(ctx, ctx->pending, SECTOR_SIZE);
            ctx->pending_len = 0;
        }
        if (rc == 0 && ctx->offset < ctx->image_size) rc = -EIO;
        if (rc == 0) rc = write_hdl_info(ctx);
        if (rc == 0) rc = patch_apa_type(ctx);
        fileXioDevctl("hdd0:", HDIOC_FLUSH, NULL, 0, NULL, 0);
    } else {
        fileXioRemove(ctx->target_path);
    }

    if (rc != 0) fileXioRemove(ctx->target_path);
    ctx->active = false;
    return rc;
}

int hdl_remove_partition(const char *path) {
    if (!path || !path[0]) return -EINVAL;
    return fileXioRemove(path);
}

static void copy_limited(char *dst, size_t dst_size,
                         const char *src, size_t src_size)
{
    if (!dst || dst_size == 0) return;
    size_t n = 0;
    while (n + 1 < dst_size && n < src_size && src[n]) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

static uint64_t allocated_usable_size(const HdlApaHeader *header) {
    uint64_t total =
        partition_usable_bytes(header->length, HDL_MAIN_DATA_OFFSET_SECTORS);
    int nsub = (header->nsub > APA_MAXSUB) ? APA_MAXSUB : (int)header->nsub;
    for (int i = 0; i < nsub; i++) {
        total += partition_usable_bytes(header->subs[i].length,
                                        HDL_SUB_DATA_OFFSET_SECTORS);
    }
    return total;
}

static void fill_local_from_header(const HdlApaHeader *header,
                                   LocalRom *out)
{
    memset(out, 0, sizeof(*out));
    copy_limited(out->filename, sizeof(out->filename),
                 header->id, APA_IDMAX);
    snprintf(out->path, sizeof(out->path), "hdd0:%s", out->filename);
    out->size = allocated_usable_size(header);

    if (raw_read_sectors(header->start + HDL_INFO_OFFSET_SECTORS,
                         sizeof(g_info_buf) / SECTOR_SIZE,
                         &g_info_buf) == 0 &&
        g_info_buf.checksum == HDL_MAGIC)
    {
        copy_limited(out->name, sizeof(out->name),
                     g_info_buf.gamename, sizeof(g_info_buf.gamename));
        copy_limited(out->serial, sizeof(out->serial),
                     g_info_buf.startup, sizeof(g_info_buf.startup));
        out->is_cd = (g_info_buf.discType == HDL_DISC_CD);
        return;
    }

    strncpy(out->name, out->filename, sizeof(out->name) - 1);
    strncpy(out->serial, "", sizeof(out->serial) - 1);
    out->is_cd = false;
}

void hdl_scan_local(LocalRomList *out) {
    if (!out) return;
    out->count = 0;
    out->last_error[0] = '\0';

    uint32_t lba = 0;
    for (int guard = 0; guard < 4096 && out->count < LOCAL_ROMS_MAX; guard++) {
        if (raw_read_sectors(lba, sizeof(HdlApaHeader) / SECTOR_SIZE,
                             &g_apa_buf) != 0) {
            snprintf(out->last_error, sizeof(out->last_error),
                     "Could not read APA partition table");
            return;
        }
        if (g_apa_buf.magic != APA_MAGIC) {
            snprintf(out->last_error, sizeof(out->last_error),
                     "HDD is not APA-formatted");
            return;
        }

        if (!(g_apa_buf.flags & APA_FLAG_SUB) &&
            g_apa_buf.type == APA_TYPE_HDL) {
            fill_local_from_header(&g_apa_buf, &out->items[out->count]);
            out->count++;
        }

        if (g_apa_buf.next == 0 || g_apa_buf.next == lba) break;
        lba = g_apa_buf.next;
    }

    if (out->count == 0) {
        snprintf(out->last_error, sizeof(out->last_error),
                 "No HDLoader partitions found on hdd0:");
    }
}
