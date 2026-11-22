#ifndef AHCI_H
#define AHCI_H

/* Kernel side of the AHCI (SATA) driver. struct ahci_device and the
 * listing calls are in seaweed.h. */

#include "seaweed.h"

/* Finds the first AHCI controller on PCI, takes it over from the
 * firmware, and identifies whatever is plugged into its ports. Call
 * after pci_init(), with interrupts on (the waits use the timer).
 * Returns the number of devices found, or -1 if there is no controller
 * or it could not be reached. */
int ahci_init(void);

/* The controller's AHCI version register: 0x00010000 is 1.0,
 * 0x00010301 is 1.3.1. */
uint32_t ahci_version(void);

/* Reading and writing whole 512-byte sectors on disk 'index' (as listed
 * by ahci_get). Large transfers are split into several commands, and a
 * write is followed by a cache flush. 'buffer' must be an even address
 * in ordinary kernel memory, because the controller copies to and from
 * it by itself (DMA).
 *
 * Kernel only: nothing above Seaweed may write raw sectors - Fish goes
 * through the partition layer, which keeps it inside its partition. */
#define AHCI_OK            0
#define AHCI_ERR_NODEV    -1   /* no such disk, or not a disk */
#define AHCI_ERR_RANGE    -2   /* past the end of the disk */
#define AHCI_ERR_BUFFER   -3   /* the buffer cannot be used for DMA */
#define AHCI_ERR_DEVICE   -4   /* the disk reported an error */
#define AHCI_ERR_TIMEOUT  -5   /* it did not finish in time */

int ahci_read(int index, uint64_t lba, uint32_t count, void *buffer);
int ahci_write(int index, uint64_t lba, uint32_t count, const void *buffer);
const char *ahci_error(int code);

#endif
