#ifndef BLOCK_H
#define BLOCK_H

/* One way to reach every disk, whatever is underneath: the old IDE
 * driver or AHCI. The partition layer - and so Fish - talks to whichever
 * disk is selected, and never to a driver directly.
 *
 * The listing (struct block_info, block_count, block_get, block_current)
 * is public, in seaweed.h. */

#include "seaweed.h"

#define BLOCK_SECTOR_SIZE 512

/* Collects every disk the drivers found: the IDE primary master first,
 * then SATA disks in port order. CD/DVD drives are not disks here.
 * Call after ata_init() and ahci_init(). Selects nothing. */
void block_init(void);

/* Makes disk 'index' the one the calls below work on. Returns 0, or -1
 * if there is no such disk. */
int block_select(int index);

/* 1 if a disk is selected. */
int block_present(void);

/* How many sectors of the selected disk the partition layer may use.
 * Capped at what 32-bit sector numbers can reach (2 TiB), and at 128 GiB
 * for the IDE driver. */
uint32_t block_sectors(void);

/* Whole sectors on the selected disk. Return 0, or -1 on any failure. */
int block_read(uint32_t lba, uint32_t count, void *buffer);
int block_write(uint32_t lba, uint32_t count, const void *buffer);

#endif
