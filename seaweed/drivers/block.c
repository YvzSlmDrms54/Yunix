/* seaweed/drivers/block.c - every disk behind one interface.
 *
 * Seaweed has two disk drivers: the old IDE one (ata.c) for the primary
 * master, and AHCI (ahci.c) for SATA disks. Neither the partition code
 * nor Fish should care which one a disk hangs off, so this file lists
 * the disks both drivers found and passes reads and writes for the
 * selected one to the right driver.
 */

#include "block.h"
#include "ata.h"
#include "ahci.h"

#define LBA28_LIMIT   0x10000000u
#define LBA32_LIMIT   0xFFFFFFFFu

struct disk {
    struct block_info info;
    int               ahci_index;     /* for SATA disks: index in ahci_get */
};

static struct disk disks[BLOCK_MAX];
static int         count;
static int         current = -1;

void block_init(void)
{
    count = 0;
    current = -1;

    if (ata_present()) {
        struct disk *d = &disks[count++];

        d->info.kind = BLOCK_IDE;
        d->info.port = 0;
        d->info.sectors = ata_sectors();
        d->info.model = "IDE primary master";
        d->ahci_index = -1;
    }

    for (int i = 0; i < ahci_count() && count < BLOCK_MAX; i++) {
        const struct ahci_device *a = ahci_get(i);
        struct disk *d;

        if (a->kind != AHCI_DISK || !a->identified || a->sectors == 0) continue;

        d = &disks[count++];
        d->info.kind = BLOCK_SATA;
        d->info.port = a->port;
        d->info.sectors = a->sectors;
        d->info.model = a->model[0] != '\0' ? a->model : "SATA disk";
        d->ahci_index = i;
    }
}

int block_count(void)
{
    return count;
}

int block_get(int index, struct block_info *out)
{
    if (index < 0 || index >= count) return -1;
    *out = disks[index].info;
    return 0;
}

int block_current(void)
{
    return current;
}

int block_select(int index)
{
    if (index < 0 || index >= count) return -1;
    current = index;
    return 0;
}

int block_present(void)
{
    return current >= 0;
}

uint32_t block_sectors(void)
{
    uint64_t sectors;

    if (current < 0) return 0;
    sectors = disks[current].info.sectors;

    if (disks[current].info.kind == BLOCK_IDE && sectors > LBA28_LIMIT) return LBA28_LIMIT;
    if (sectors > LBA32_LIMIT) return LBA32_LIMIT;
    return (uint32_t)sectors;
}

static int in_reach(uint32_t lba, uint32_t n)
{
    uint32_t end = block_sectors();
    return lba < end && n <= end - lba;
}

int block_read(uint32_t lba, uint32_t n, void *buffer)
{
    uint8_t *out = buffer;

    if (current < 0 || !in_reach(lba, n)) return -1;

    if (disks[current].info.kind == BLOCK_SATA) {
        return ahci_read(disks[current].ahci_index, lba, n, buffer) == AHCI_OK ? 0 : -1;
    }

    /* The IDE driver counts sectors in a byte; one at a time is simplest. */
    for (uint32_t i = 0; i < n; i++) {
        if (ata_read(lba + i, 1, out + i * BLOCK_SECTOR_SIZE) != 0) return -1;
    }
    return 0;
}

int block_write(uint32_t lba, uint32_t n, const void *buffer)
{
    const uint8_t *in = buffer;

    if (current < 0 || !in_reach(lba, n)) return -1;

    if (disks[current].info.kind == BLOCK_SATA) {
        return ahci_write(disks[current].ahci_index, lba, n, buffer) == AHCI_OK ? 0 : -1;
    }

    for (uint32_t i = 0; i < n; i++) {
        if (ata_write(lba + i, 1, in + i * BLOCK_SECTOR_SIZE) != 0) return -1;
    }
    return 0;
}
