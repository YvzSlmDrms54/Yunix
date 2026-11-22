/* seaweed/fs/partition.c - finding Fish's own space on the disk.
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
 *        header points to an array of 128-byte entries. A Fish partition
 *        has the type GUID 7E1B0F3C-9A4D-4E2B-8C61-59554E495846 (the last
 *        six bytes spell "YUNIXF").
 *
 * The only time Seaweed writes a partition table is on a disk that is
 * completely blank - see partition_create_fish().
 */

#include "partition.h"
#include "ata.h"
#include "string.h"

#define MBR_SIGNATURE_OFFSET   510
#define MBR_TABLE_OFFSET       446
#define MBR_ENTRY_SIZE         16
#define MBR_ENTRIES            4

#define MBR_TYPE_EMPTY         0x00
#define MBR_TYPE_GPT           0xEE   /* protective entry in front of a GPT */
#define MBR_TYPE_FISH          0x7F

/* Where a new Fish partition starts: 1 MiB in, the usual place. Everything
 * before it is left to the partition table and boot loaders. */
#define FISH_FIRST_SECTOR      2048

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

/* 7E1B0F3C-9A4D-4E2B-8C61-59554E495846 as it is stored on disk. GPT keeps
 * the first three fields little-endian and the last two as plain bytes. */
static const uint8_t fish_guid[16] = {
    0x3C, 0x0F, 0x1B, 0x7E,  0x4D, 0x9A,  0x2B, 0x4E,
    0x8C, 0x61,  0x59, 0x55, 0x4E, 0x49, 0x58, 0x46
};

static enum disk_layout layout = DISK_NONE;
static const char      *table = "none";
static int              partitions;
static uint32_t         fish_start;
static uint32_t         fish_sectors;

/* --- reading little-endian numbers out of a sector --- */

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

/* Is a partition with these bounds somewhere we can actually use? */
static int usable(uint32_t start, uint32_t count, uint32_t first_allowed)
{
    uint32_t disk = ata_sectors();

    if (count == 0 || start < first_allowed) return 0;
    if (start >= disk || count > disk - start) return 0;
    if (start + count > LBA28_LIMIT) return 0;
    return 1;
}

static void found_fish(uint32_t start, uint32_t count)
{
    if (layout == DISK_FISH) return;      /* keep the first one */
    layout = DISK_FISH;
    fish_start = start;
    fish_sectors = count;
}

/* --- the two kinds of table --- */

static void scan_gpt(void)
{
    uint8_t  sector[ATA_SECTOR_SIZE];
    uint32_t entries_lba, count, size, per_sector;

    table = "GPT";

    if (ata_read(1, 1, sector) != 0 || memcmp(sector, "EFI PART", 8) != 0) {
        table = "GPT (damaged)";
        return;
    }

    if (!get64_small(sector + 72, &entries_lba)) return;
    count = get32(sector + 80);
    size = get32(sector + 84);

    /* Entries are 128 bytes on every disk we are likely to meet. Accept
     * anything that divides a sector evenly; refuse the rest. */
    if (size < 128 || size > ATA_SECTOR_SIZE || ATA_SECTOR_SIZE % size != 0) {
        table = "GPT (unsupported)";
        return;
    }
    if (count > 256) count = 256;
    per_sector = ATA_SECTOR_SIZE / size;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry;
        uint32_t first, last;
        int empty = 1;

        if (i % per_sector == 0) {
            if (ata_read(entries_lba + i / per_sector, 1, sector) != 0) return;
        }
        entry = sector + (i % per_sector) * size;

        for (int b = 0; b < 16; b++) if (entry[b] != 0) { empty = 0; break; }
        if (empty) continue;

        partitions++;

        if (memcmp(entry, fish_guid, 16) != 0) continue;
        if (!get64_small(entry + 32, &first) || !get64_small(entry + 40, &last)) continue;
        if (last < first) continue;

        /* GPT's own structures fill sectors 0-33. */
        if (usable(first, last - first + 1, 34)) found_fish(first, last - first + 1);
    }
}

static void scan_mbr(const uint8_t *sector)
{
    table = "MBR";

    for (int i = 0; i < MBR_ENTRIES; i++) {
        const uint8_t *entry = sector + MBR_TABLE_OFFSET + i * MBR_ENTRY_SIZE;
        uint8_t  type  = entry[4];
        uint32_t start = get32(entry + 8);
        uint32_t count = get32(entry + 12);

        if (type == MBR_TYPE_EMPTY) continue;

        if (type == MBR_TYPE_GPT) {
            /* The MBR is only a guard; the real table is the GPT. */
            partitions = 0;
            scan_gpt();
            return;
        }

        partitions++;
        if (type == MBR_TYPE_FISH && usable(start, count, 1)) found_fish(start, count);
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

/* --- public --- */

enum disk_layout partition_scan(void)
{
    uint8_t sector[ATA_SECTOR_SIZE];

    layout = DISK_NONE;
    table = "none";
    partitions = 0;
    fish_start = 0;
    fish_sectors = 0;

    if (!ata_present()) return layout;

    /* Anything we do not recognise as ours is someone else's. Only a
     * Fish partition, or a disk with nothing at all on it, changes that. */
    layout = DISK_FOREIGN;

    if (ata_read(0, 1, sector) != 0) return layout;

    if (sector[MBR_SIGNATURE_OFFSET] == 0x55 && sector[MBR_SIGNATURE_OFFSET + 1] == 0xAA) {
        scan_mbr(sector);
    } else if (disk_is_blank()) {
        layout = DISK_BLANK;
    }

    return layout;
}

int partition_create_fish(void)
{
    uint8_t  sector[ATA_SECTOR_SIZE];
    uint8_t *entry = sector + MBR_TABLE_OFFSET;
    uint32_t disk = ata_sectors();

    /* Look again right before writing: this must never run on a disk
     * that holds anything. */
    if (partition_scan() != DISK_BLANK) return -1;
    if (disk > LBA28_LIMIT) disk = LBA28_LIMIT;
    if (disk <= FISH_FIRST_SECTOR) return -1;

    memset(sector, 0, sizeof(sector));
    put32(sector + 440, MBR_DISK_SIGNATURE);

    /* One entry. The CHS fields are from the days of cylinders and heads;
     * FE FF FF is the usual way of saying "use the LBA fields instead". */
    entry[0] = 0x00;                                  /* not bootable */
    entry[1] = 0xFE; entry[2] = 0xFF; entry[3] = 0xFF;
    entry[4] = MBR_TYPE_FISH;
    entry[5] = 0xFE; entry[6] = 0xFF; entry[7] = 0xFF;
    put32(entry + 8, FISH_FIRST_SECTOR);
    put32(entry + 12, disk - FISH_FIRST_SECTOR);

    sector[MBR_SIGNATURE_OFFSET] = 0x55;
    sector[MBR_SIGNATURE_OFFSET + 1] = 0xAA;

    if (ata_write(0, 1, sector) != 0) return -1;

    return partition_scan() == DISK_FISH ? 0 : -1;
}

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

void disk_info(struct disk_info *out)
{
    out->layout       = layout;
    out->sectors      = ata_sectors();
    out->table        = table;
    out->partitions   = partitions;
    out->fish_start   = fish_start;
    out->fish_sectors = fish_sectors;
}
