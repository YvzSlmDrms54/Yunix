/* seaweed/fs/partition.c - finding, and making, Fish's own space on the disk.
 *
 * A real disk is shared: Windows, Linux or anything else may live on it,
 * each in its own partition, listed in a partition table at the start of
 * the disk. Fish must only ever write inside a partition that is marked
 * as Fish's, so this file reads the table, finds that partition, and is
 * the only path Fish has to the disk: every read and write goes through
 * partition_read/partition_write, which refuse anything outside it.
 *
 * Two kinds of table are understood:
 *
 *   MBR  The original PC table, in sector 0: four 16-byte entries at byte
 *        446 and the signature 55 AA at byte 510. A Fish partition has
 *        type 0x7F, which is set aside for hobby and experimental systems.
 *
 *   GPT  The modern one. Sector 0 holds a "protective" MBR with a single
 *        entry of type 0xEE, sector 1 a header starting "EFI PART", and the
 *        header points to an array of 128-byte entries. Both are protected
 *        by CRC32 checksums, and a second copy of both sits at the very
 *        end of the disk. A Fish partition has the type GUID
 *        7E1B0F3C-9A4D-4E2B-8C61-59554E495846 (the last six bytes spell
 *        "YUNIXF").
 *
 * Seaweed changes a partition table in exactly one way: it adds a single
 * Fish entry over space that no partition uses (disk_create_fish). It
 * never moves, shrinks or removes anything. If anything about the table
 * does not add up, it refuses to change it at all.
 */

#include "partition.h"
#include "ata.h"
#include "heap.h"
#include "string.h"

#define MBR_SIGNATURE_OFFSET   510
#define MBR_TABLE_OFFSET       446
#define MBR_ENTRY_SIZE         16
#define MBR_ENTRIES            4

#define MBR_TYPE_EMPTY         0x00
#define MBR_TYPE_GPT           0xEE   /* protective entry in front of a GPT */
#define MBR_TYPE_FISH          0x7F

/* Partitions start on 1 MiB boundaries, as every modern tool lays them
 * out. It keeps them aligned to the erase blocks of an SSD, and leaves
 * the first MiB to the partition table and boot loaders. */
#define ALIGN_SECTORS          2048
#define MIN_REGION_SECTORS     2048    /* smaller gaps are not worth listing */

/* How much of a disk has to be zero for it to count as blank. 129 sectors
 * reach past every place a common filesystem or table keeps its marker:
 * the MBR and FAT/NTFS/exFAT boot sectors (0), the GPT header and entries
 * (1-33), the ext2/3/4 superblock (2), ISO 9660 (64) and btrfs (128). */
#define BLANK_CHECK_SECTORS    129

/* Arbitrary, but must not be zero: some systems treat a zero disk
 * signature as "not initialised". The bytes spell "YUNX". */
#define MBR_DISK_SIGNATURE     0x584E5559u

/* 28-bit LBA cannot reach past this sector. */
#define LBA28_LIMIT            0x10000000u

#define GPT_ENTRIES_MAX        256
#define MAX_PARTS              128

/* 7E1B0F3C-9A4D-4E2B-8C61-59554E495846 as it is stored on disk. GPT keeps
 * the first three fields little-endian and the last two as plain bytes. */
static const uint8_t fish_guid[16] = {
    0x3C, 0x0F, 0x1B, 0x7E,  0x4D, 0x9A,  0x2B, 0x4E,
    0x8C, 0x61,  0x59, 0x55, 0x4E, 0x49, 0x58, 0x46
};

struct part {
    uint32_t    start;
    uint32_t    sectors;
    const char *kind;
};

enum table_kind { TABLE_NONE, TABLE_MBR, TABLE_GPT };

static enum disk_layout layout = DISK_NONE;
static enum table_kind  kind;
static const char      *table = "none";
static struct part      parts[MAX_PARTS];
static int              partitions;
static int              understood;      /* every entry made sense */
static uint32_t         fish_start;
static uint32_t         fish_sectors;

/* GPT only: the range partitions may use, from the header. */
static uint32_t         gpt_first_usable;
static uint32_t         gpt_last_usable;

/* --- small helpers --- */

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* A 64-bit field, but we can only use it if it fits in 32 bits.
 * Returns 0 if it does not. */
static int get64_small(const uint8_t *p, uint32_t *out)
{
    if (get32(p + 4) != 0) return 0;
    *out = get32(p);
    return 1;
}

static void put64(uint8_t *p, uint32_t value)
{
    put32(p, value);
    put32(p + 4, 0);
}

/* The disk as far as we can reach it. */
static uint32_t disk_end(void)
{
    uint32_t disk = ata_sectors();
    return disk > LBA28_LIMIT ? LBA28_LIMIT : disk;
}

/* The CRC32 GPT uses - the same one as zip and Ethernet. Bit by bit
 * rather than from a table: it runs a handful of times per boot. */
static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static int read_sectors(uint32_t lba, uint32_t count, uint8_t *buffer)
{
    for (uint32_t i = 0; i < count; i++) {
        if (ata_read(lba + i, 1, buffer + i * ATA_SECTOR_SIZE) != 0) return -1;
    }
    return 0;
}

static int write_sectors(uint32_t lba, uint32_t count, const uint8_t *buffer)
{
    for (uint32_t i = 0; i < count; i++) {
        if (ata_write(lba + i, 1, buffer + i * ATA_SECTOR_SIZE) != 0) return -1;
    }
    return 0;
}

/* Records one partition. Anything that runs past the end of the disk, or
 * is empty, means we do not understand this table well enough to change
 * it - but it is still listed, and still counts as somebody's. */
static void add_part(uint32_t start, uint32_t count, const char *what, int is_fish,
                     uint32_t first_allowed)
{
    uint32_t end = disk_end();
    int sane = count > 0 && start >= first_allowed && start < end && count <= end - start;

    if (!sane) understood = 0;

    if (partitions < MAX_PARTS) {
        parts[partitions].start = start;
        parts[partitions].sectors = count;
        parts[partitions].kind = what;
    } else {
        understood = 0;              /* more than we can keep track of */
    }
    partitions++;

    if (is_fish && sane && layout != DISK_FISH) {
        layout = DISK_FISH;
        fish_start = start;
        fish_sectors = count;
    }
}

/* --- naming partitions, for the listing --- */

static const char *mbr_kind(uint8_t type)
{
    switch (type) {
    case 0x01: case 0x04: case 0x06: case 0x0B: case 0x0C: case 0x0E:
        return "FAT";
    case 0x05: case 0x0F: case 0x85:
        return "extended";
    case 0x07: return "NTFS / exFAT";
    case 0x27: return "Windows recovery";
    case 0x7F: return "Fish";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux filesystem";
    case 0x8E: return "Linux LVM";
    case 0xEF: return "EFI system";
    default:   return "other";
    }
}

struct known_guid {
    const char *name;
    uint8_t     bytes[16];
};

/* The handful of GPT types anyone is likely to meet, in on-disk order. */
static const struct known_guid gpt_kinds[] = {
    { "EFI system",          { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                               0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } },
    { "Microsoft basic data",{ 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                               0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 } },
    { "Microsoft reserved",  { 0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D,
                               0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } },
    { "Windows recovery",    { 0xA4, 0xBB, 0x94, 0xDE, 0xD1, 0x06, 0x40, 0x4D,
                               0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC } },
    { "Linux filesystem",    { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                               0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 } },
    { "Linux swap",          { 0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43,
                               0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F } },
};

static const char *gpt_kind(const uint8_t *guid)
{
    if (memcmp(guid, fish_guid, 16) == 0) return "Fish";
    for (size_t i = 0; i < sizeof(gpt_kinds) / sizeof(gpt_kinds[0]); i++) {
        if (memcmp(guid, gpt_kinds[i].bytes, 16) == 0) return gpt_kinds[i].name;
    }
    return "other";
}

/* --- GPT --- */

/* Everything about one copy of the GPT: its header, and its entries. */
struct gpt {
    uint8_t  header[ATA_SECTOR_SIZE];
    uint32_t header_lba;
    uint32_t header_size;
    uint32_t backup_lba;     /* where the header says the other copy is */
    uint32_t entries_lba;
    uint32_t count;
    uint32_t entry_size;
    uint32_t entries_sectors;
    uint8_t *entries;        /* kmalloc'd, entries_sectors * 512 bytes */
};

/* Reads and checks one copy. Returns 0 if it is valid - signature, sizes,
 * both checksums - and fills 'g'. The caller frees g->entries. */
static int gpt_read(uint32_t lba, struct gpt *g)
{
    uint8_t  saved[4];
    uint32_t bytes;

    g->entries = 0;
    g->header_lba = lba;

    if (lba >= disk_end()) return -1;
    if (ata_read(lba, 1, g->header) != 0) return -1;
    if (memcmp(g->header, "EFI PART", 8) != 0) return -1;

    g->header_size = get32(g->header + 12);
    if (g->header_size < 92 || g->header_size > ATA_SECTOR_SIZE) return -1;

    /* The header checksum is taken with its own field set to zero. */
    memcpy(saved, g->header + 16, 4);
    memset(g->header + 16, 0, 4);
    if (crc32(g->header, g->header_size) != get32(saved)) return -1;
    memcpy(g->header + 16, saved, 4);

    if (!get64_small(g->header + 32, &g->backup_lba))  return -1;
    if (!get64_small(g->header + 72, &g->entries_lba)) return -1;
    g->count = get32(g->header + 80);
    g->entry_size = get32(g->header + 84);

    if (g->entry_size < 128 || g->entry_size > ATA_SECTOR_SIZE ||
        ATA_SECTOR_SIZE % g->entry_size != 0) return -1;
    if (g->count == 0 || g->count > GPT_ENTRIES_MAX) return -1;

    bytes = g->count * g->entry_size;
    g->entries_sectors = (bytes + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    if (g->entries_lba + g->entries_sectors > disk_end()) return -1;

    g->entries = kcalloc(g->entries_sectors, ATA_SECTOR_SIZE);
    if (g->entries == 0) return -1;

    if (read_sectors(g->entries_lba, g->entries_sectors, g->entries) != 0 ||
        crc32(g->entries, bytes) != get32(g->header + 88)) {
        kfree(g->entries);
        g->entries = 0;
        return -1;
    }
    return 0;
}

static int entry_is_empty(const uint8_t *entry)
{
    for (int b = 0; b < 16; b++) if (entry[b] != 0) return 0;
    return 1;
}

static void scan_gpt(void)
{
    struct gpt g;

    kind = TABLE_GPT;
    table = "GPT";

    if (gpt_read(1, &g) != 0) {
        table = "GPT (damaged)";
        understood = 0;
        return;
    }

    if (!get64_small(g.header + 40, &gpt_first_usable) ||
        !get64_small(g.header + 48, &gpt_last_usable) ||
        gpt_last_usable < gpt_first_usable) {
        understood = 0;
    }

    for (uint32_t i = 0; i < g.count; i++) {
        const uint8_t *entry = g.entries + i * g.entry_size;
        uint32_t first, last;

        if (entry_is_empty(entry)) continue;

        if (!get64_small(entry + 32, &first) || !get64_small(entry + 40, &last) ||
            last < first) {
            /* Somewhere we cannot reach or make sense of. It is still
             * somebody's partition; add_part marks the table as not
             * understood, so nothing will be built around it. */
            add_part(0, 0, "out of reach", 0, 34);
            continue;
        }

        /* GPT's own structures fill sectors 0-33. */
        add_part(first, last - first + 1, gpt_kind(entry),
                 memcmp(entry, fish_guid, 16) == 0, 34);
    }

    kfree(g.entries);
}

/* --- MBR --- */

static void scan_mbr(const uint8_t *sector)
{
    kind = TABLE_MBR;
    table = "MBR";

    for (int i = 0; i < MBR_ENTRIES; i++) {
        const uint8_t *entry = sector + MBR_TABLE_OFFSET + i * MBR_ENTRY_SIZE;
        uint8_t type = entry[4];

        if (type == MBR_TYPE_EMPTY) continue;

        if (type == MBR_TYPE_GPT) {
            /* The MBR is only a guard; the real table is the GPT. */
            partitions = 0;
            understood = 1;
            scan_gpt();
            return;
        }

        add_part(get32(entry + 8), get32(entry + 12), mbr_kind(type),
                 type == MBR_TYPE_FISH, 1);
    }
}

/* Is the start of the disk entirely zeros? */
static int disk_is_blank(void)
{
    uint8_t sector[ATA_SECTOR_SIZE];

    for (uint32_t s = 0; s < BLANK_CHECK_SECTORS; s++) {
        if (ata_read(s, 1, sector) != 0) return 0;     /* unreadable is not blank */
        for (int i = 0; i < ATA_SECTOR_SIZE; i++) if (sector[i] != 0) return 0;
    }
    return 1;
}

/* --- scanning --- */

enum disk_layout partition_scan(void)
{
    uint8_t sector[ATA_SECTOR_SIZE];

    layout = DISK_NONE;
    kind = TABLE_NONE;
    table = "none";
    partitions = 0;
    understood = 1;
    fish_start = 0;
    fish_sectors = 0;
    gpt_first_usable = 0;
    gpt_last_usable = 0;

    if (!ata_present()) return layout;

    /* Anything we do not recognise as ours is someone else's. Only a
     * Fish partition, or a disk with nothing at all on it, changes that. */
    layout = DISK_FOREIGN;

    if (ata_read(0, 1, sector) != 0) { understood = 0; return layout; }

    if (sector[MBR_SIGNATURE_OFFSET] == 0x55 && sector[MBR_SIGNATURE_OFFSET + 1] == 0xAA) {
        scan_mbr(sector);
    } else if (disk_is_blank()) {
        layout = DISK_BLANK;
    } else {
        understood = 0;      /* data, but no table we could add to */
    }

    return layout;
}

/* --- free space --- */

/* The range partitions may live in, end exclusive. */
static int usable_range(uint32_t *low, uint32_t *high)
{
    uint32_t end = disk_end();

    if (!understood) return 0;

    switch (kind) {
    case TABLE_NONE:
        if (layout != DISK_BLANK) return 0;
        *low = ALIGN_SECTORS;
        *high = end;
        return 1;
    case TABLE_MBR:
        *low = ALIGN_SECTORS;
        *high = end;
        return 1;
    case TABLE_GPT:
        /* The backup GPT lives at the very end of the disk, so the header
         * must say where the usable space ends. If that end is out of our
         * reach, so is the backup we would have to update. */
        if (gpt_last_usable >= end) return 0;
        *low = gpt_first_usable;
        *high = gpt_last_usable + 1;
        return 1;
    }
    return 0;
}

int disk_free_space(struct disk_region *out, int max)
{
    uint32_t low, high, cursor, max_end = 0;
    int order[MAX_PARTS];
    int count = 0;

    if (!usable_range(&low, &high)) return 0;

    /* Sort the partitions by where they start (insertion sort: there are
     * rarely more than a handful). */
    for (int i = 0; i < partitions; i++) {
        int j = i;
        while (j > 0 && parts[order[j - 1]].start > parts[i].start) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }

    /* Walk through them, collecting the gaps. Two partitions that overlap
     * mean the table is wrong somewhere - in which case we offer nothing. */
    cursor = low;
    for (int k = 0; k <= partitions; k++) {
        uint32_t gap_end = (k < partitions) ? parts[order[k]].start : high;
        uint32_t start;

        if (k < partitions && parts[order[k]].start < max_end) return 0;

        if (gap_end > high) gap_end = high;

        start = (cursor + ALIGN_SECTORS - 1) / ALIGN_SECTORS * ALIGN_SECTORS;
        if (gap_end > start && gap_end - start >= MIN_REGION_SECTORS) {
            if (count < max) {
                out[count].start = start;
                out[count].sectors = gap_end - start;
            }
            count++;
        }

        if (k < partitions) {
            uint32_t end = parts[order[k]].start + parts[order[k]].sectors;
            if (end > max_end) max_end = end;
            if (end > cursor) cursor = end;
        }
    }

    return count < max ? count : max;
}

/* --- adding a Fish partition --- */

static int create_on_blank(uint32_t start, uint32_t sectors)
{
    uint8_t  sector[ATA_SECTOR_SIZE];
    uint8_t *entry = sector + MBR_TABLE_OFFSET;

    memset(sector, 0, sizeof(sector));
    put32(sector + 440, MBR_DISK_SIGNATURE);

    /* One entry. The CHS fields are from the days of cylinders and heads;
     * FE FF FF is the usual way of saying "use the LBA fields instead". */
    entry[0] = 0x00;                                  /* not bootable */
    entry[1] = 0xFE; entry[2] = 0xFF; entry[3] = 0xFF;
    entry[4] = MBR_TYPE_FISH;
    entry[5] = 0xFE; entry[6] = 0xFF; entry[7] = 0xFF;
    put32(entry + 8, start);
    put32(entry + 12, sectors);

    sector[MBR_SIGNATURE_OFFSET] = 0x55;
    sector[MBR_SIGNATURE_OFFSET + 1] = 0xAA;

    return ata_write(0, 1, sector) == 0 ? DISK_OK : DISK_ERR_IO;
}

/* Fills one empty slot in an existing MBR. Sector 0 is read back fresh,
 * and only those 16 bytes change: the boot code, the disk signature and
 * every other entry are written back exactly as they were. */
static int create_in_mbr(uint32_t start, uint32_t sectors)
{
    uint8_t sector[ATA_SECTOR_SIZE];

    if (ata_read(0, 1, sector) != 0) return DISK_ERR_IO;

    for (int i = 0; i < MBR_ENTRIES; i++) {
        uint8_t *entry = sector + MBR_TABLE_OFFSET + i * MBR_ENTRY_SIZE;

        if (entry[4] != MBR_TYPE_EMPTY) continue;

        memset(entry, 0, MBR_ENTRY_SIZE);
        entry[1] = 0xFE; entry[2] = 0xFF; entry[3] = 0xFF;
        entry[4] = MBR_TYPE_FISH;
        entry[5] = 0xFE; entry[6] = 0xFF; entry[7] = 0xFF;
        put32(entry + 8, start);
        put32(entry + 12, sectors);

        return ata_write(0, 1, sector) == 0 ? DISK_OK : DISK_ERR_IO;
    }

    /* Extended partitions could hold more, but four is where plain MBR
     * stops, and Seaweed does not create extended partitions. */
    return DISK_ERR_TABLE_FULL;
}

/* A new, unique GUID for the partition itself (not its type). The CPU's
 * time-stamp counter, which ticks every clock cycle, is as unpredictable
 * as anything we have; xorshift spreads it over sixteen bytes. */
static void make_guid(uint8_t *out)
{
    uint32_t low, high, state;

    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    state = low ^ (high * 2654435761u) ^ timer_ticks() ^ 0x59554E58u;
    if (state == 0) state = 1;

    for (int i = 0; i < 16; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        out[i] = (uint8_t)(state >> 24);
    }

    out[7] = (uint8_t)((out[7] & 0x0F) | 0x40);   /* version 4: random */
    out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);   /* the standard variant */
}

/* Recomputes a header's checksum after a change. */
static void gpt_seal(struct gpt *g)
{
    memset(g->header + 16, 0, 4);
    put32(g->header + 16, crc32(g->header, g->header_size));
}

/* Adds an entry to both copies of the GPT. Both are read and checked
 * first; if either is damaged, nothing is written. */
static int create_in_gpt(uint32_t start, uint32_t sectors)
{
    static const char name[] = "Yunix Fish";
    struct gpt primary, backup;
    uint8_t *entry = 0;
    uint32_t entries_crc;
    int result = DISK_ERR_DAMAGED;

    backup.entries = 0;
    if (gpt_read(1, &primary) != 0) return DISK_ERR_DAMAGED;
    if (gpt_read(primary.backup_lba, &backup) != 0) goto done;

    /* The two copies must describe the same table. */
    if (backup.count != primary.count || backup.entry_size != primary.entry_size ||
        memcmp(backup.entries, primary.entries, primary.count * primary.entry_size) != 0) {
        goto done;
    }

    for (uint32_t i = 0; i < primary.count; i++) {
        uint8_t *candidate = primary.entries + i * primary.entry_size;
        if (entry_is_empty(candidate)) { entry = candidate; break; }
    }
    if (entry == 0) { result = DISK_ERR_TABLE_FULL; goto done; }

    /* Fill in the entry: type, its own GUID, first and last sector
     * (inclusive), no attributes, and a name in UTF-16. */
    memset(entry, 0, primary.entry_size);
    memcpy(entry, fish_guid, 16);
    make_guid(entry + 16);
    put64(entry + 32, start);
    put64(entry + 40, start + sectors - 1);
    for (size_t c = 0; name[c] != '\0'; c++) entry[56 + c * 2] = (uint8_t)name[c];

    /* The backup's entries are the same array. */
    memcpy(backup.entries, primary.entries, primary.count * primary.entry_size);

    entries_crc = crc32(primary.entries, primary.count * primary.entry_size);
    put32(primary.header + 88, entries_crc);
    put32(backup.header + 88, entries_crc);
    gpt_seal(&primary);
    gpt_seal(&backup);

    /* Primary first, then the backup. If the power goes in between, one
     * copy is the old table and the other the new one - each valid on its
     * own, which is what having two copies is for. */
    result = DISK_ERR_IO;
    if (write_sectors(primary.entries_lba, primary.entries_sectors, primary.entries) != 0) goto done;
    if (ata_write(primary.header_lba, 1, primary.header) != 0) goto done;
    if (write_sectors(backup.entries_lba, backup.entries_sectors, backup.entries) != 0) goto done;
    if (ata_write(backup.header_lba, 1, backup.header) != 0) goto done;
    result = DISK_OK;

done:
    kfree(primary.entries);
    kfree(backup.entries);
    return result;
}

int disk_create_fish(uint32_t start, uint32_t sectors)
{
    struct disk_region regions[MAX_PARTS + 1];
    uint32_t low, high;
    int count, inside = 0;
    int result;

    /* Look at the disk again, right now, rather than trusting what we
     * saw at boot. */
    partition_scan();

    if (layout == DISK_NONE) return DISK_ERR_NODISK;
    if (layout == DISK_FISH) return DISK_ERR_HAS_FISH;
    if (!understood) {
        return (kind == TABLE_NONE) ? DISK_ERR_UNSUPPORTED : DISK_ERR_DAMAGED;
    }
    if (!usable_range(&low, &high)) return DISK_ERR_UNSUPPORTED;
    if (sectors < MIN_REGION_SECTORS) return DISK_ERR_TOO_SMALL;
    if (start % ALIGN_SECTORS != 0 || start + sectors < start) return DISK_ERR_NOT_FREE;

    /* The new partition must sit wholly inside one stretch of free space. */
    count = disk_free_space(regions, MAX_PARTS + 1);
    for (int i = 0; i < count; i++) {
        if (start >= regions[i].start &&
            start + sectors <= regions[i].start + regions[i].sectors) inside = 1;
    }
    if (!inside) return DISK_ERR_NOT_FREE;

    switch (kind) {
    case TABLE_NONE: result = create_on_blank(start, sectors); break;
    case TABLE_MBR:  result = create_in_mbr(start, sectors);   break;
    case TABLE_GPT:  result = create_in_gpt(start, sectors);   break;
    default:         result = DISK_ERR_UNSUPPORTED;            break;
    }
    if (result != DISK_OK) return result;

    return partition_scan() == DISK_FISH ? DISK_OK : DISK_ERR_IO;
}

int partition_create_fish(void)
{
    uint32_t end = disk_end();

    if (end <= ALIGN_SECTORS) return -1;
    return disk_create_fish(ALIGN_SECTORS, end - ALIGN_SECTORS) == DISK_OK ? 0 : -1;
}

const char *disk_error(int code)
{
    switch (code) {
    case DISK_OK:              return "ok";
    case DISK_ERR_NODISK:      return "no disk found";
    case DISK_ERR_HAS_FISH:    return "the disk already has a Fish partition";
    case DISK_ERR_NOT_FREE:    return "that space is not free";
    case DISK_ERR_TABLE_FULL:  return "the partition table has no empty slot";
    case DISK_ERR_DAMAGED:     return "the partition table does not add up; leaving it alone";
    case DISK_ERR_UNSUPPORTED: return "the disk holds data but no table Yunix can add to";
    case DISK_ERR_IO:          return "disk error";
    case DISK_ERR_TOO_SMALL:   return "a Fish partition needs at least 1 MiB";
    default:                   return "unknown error";
    }
}

/* --- reading and writing inside the Fish partition --- */

uint32_t partition_size(void)
{
    return layout == DISK_FISH ? fish_sectors : 0;
}

int partition_read(uint32_t sector, void *buffer)
{
    if (layout != DISK_FISH || sector >= fish_sectors) return -1;
    return ata_read(fish_start + sector, 1, buffer);
}

int partition_write(uint32_t sector, const void *buffer)
{
    /* The one rule this whole file exists for. */
    if (layout != DISK_FISH || sector >= fish_sectors) return -1;
    return ata_write(fish_start + sector, 1, buffer);
}

/* --- reporting --- */

void disk_info(struct disk_info *out)
{
    out->layout       = layout;
    out->sectors      = ata_sectors();
    out->table        = table;
    out->partitions   = partitions;
    out->fish_start   = fish_start;
    out->fish_sectors = fish_sectors;
    out->understood   = understood;
}

int disk_partitions(struct disk_partition *out, int max)
{
    int n = partitions < MAX_PARTS ? partitions : MAX_PARTS;

    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        out[i].start = parts[i].start;
        out[i].sectors = parts[i].sectors;
        out[i].kind = parts[i].kind;
    }
    return n;
}
