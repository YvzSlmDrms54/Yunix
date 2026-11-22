#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* How much memory paging_init() maps, starting at address 0. Everything
 * Seaweed touches today lives well inside it: the BIOS area and the VGA
 * screen below 1 MiB, then the kernel itself, its heap and Fish's node
 * pool from 1 MiB up. An address above this is a page fault. */
#define PAGING_MAPPED_MB 16

/* Builds the page tables and turns paging on. Every mapped address still
 * points to the same physical address ("identity mapping"), so nothing
 * moves - except page 0, which is left out on purpose so that using a
 * NULL pointer faults instead of quietly reading or writing address 0.
 *
 * Call after idt_init(), so a page fault has somewhere to go. */
void paging_init(void);

/* The address that caused the last page fault. The CPU leaves it in the
 * CR2 register. */
uint32_t paging_fault_address(void);

#endif
