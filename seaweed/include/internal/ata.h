#ifndef ATA_H
#define ATA_H

/* Raw disk access. Only the kernel touches sectors directly; Yunix goes
 * through Fish and can only ask ata_present() (in seaweed.h). */

#include "seaweed.h"

#define ATA_SECTOR_SIZE 512

/* Looks for a disk on the primary IDE channel, master slot.
 * Returns 1 if one is there, 0 if not. */
int ata_init(void);

/* How many sectors the disk has, or 0 when there is no disk. */
uint32_t ata_sectors(void);

/* Both work in whole 512-byte sectors and address them by LBA - a plain
 * sector number counting from zero. Return 0 on success, negative on
 * failure. */
int ata_read(uint32_t lba, uint8_t count, void *buffer);
int ata_write(uint32_t lba, uint8_t count, const void *buffer);

#endif
