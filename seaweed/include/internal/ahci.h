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

#endif
