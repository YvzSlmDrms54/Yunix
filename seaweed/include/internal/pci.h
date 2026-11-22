#ifndef PCI_H
#define PCI_H

/* Kernel side of the PCI bus. struct pci_device and the listing calls
 * Yunix may use are in seaweed.h. */

#include "seaweed.h"

/* Walks every bus, device and function once and remembers what answers.
 * Only reads: nothing on the bus is changed. */
void pci_init(void);

/* Raw access to one device's configuration space. 'offset' must be a
 * multiple of 4. */
uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/* The first device of a given class/subclass/programming interface, or
 * NULL. pci_find(0x01, 0x06, 0x01) is the first AHCI controller. */
const struct pci_device *pci_find(uint8_t class_code, uint8_t subclass, uint8_t prog_if);

#endif
