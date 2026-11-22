#ifndef PARTITION_H
#define PARTITION_H

/* Finding Fish's partition, and the only way Fish reaches the disk.
 * enum disk_layout and disk_info() are public, in seaweed.h. */

#include "seaweed.h"

/* Reads the partition table. Call after ata_init(), and again after
 * anything changes the table. Returns what it found. */
enum disk_layout partition_scan(void);

/* Writes an MBR with a single Fish partition covering the disk from
 * 1 MiB onwards - but only if the disk is completely blank, which it
 * checks again itself. Returns 0 on success. */
int partition_create_fish(void);

/* Size of the Fish partition in sectors, or 0 if there is none. */
uint32_t partition_size(void);

/* One sector, numbered from the start of the Fish partition. Both refuse
 * (return -1) without a Fish partition or past its end, so Fish cannot
 * write anywhere else on the disk even by mistake. */
int partition_read(uint32_t sector, void *buffer);
int partition_write(uint32_t sector, const void *buffer);

#endif
