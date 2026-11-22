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

/* Maps a device's registers - 'size' bytes from 'physical' - with
 * caching off, at the same address, and returns that address as a
 * pointer. Returns NULL if it cannot (page 0, or out of page tables). */
void *paging_map_device(uint32_t physical, uint32_t size);

/* The two windows a ring 3 program lives in - see paging.c. */
#define PAGING_USER_CODE_START  0x08000000u
#define PAGING_USER_CODE_END    0x08400000u
#define PAGING_USER_STACK_START 0xBFC00000u
#define PAGING_USER_STACK_END   0xC0000000u

/* Maps one page of a program's memory, user-accessible and writable.
 * Both addresses must be page-aligned, and the virtual one inside a
 * window. Returns 0, or -1 if not. */
int paging_map_user(uint32_t virtual_page, uint32_t physical_page);

/* Where a program address really is, or 0 if it is not mapped. */
uint32_t paging_user_physical(uint32_t virtual_address);

/* 1 if every byte of [address, address + size) is mapped for the
 * program. System calls check every pointer a program hands them. */
int paging_user_range_ok(uint32_t address, uint32_t size);

/* Removes every program page. */
void paging_unmap_user(void);

/* The address that caused the last page fault. The CPU leaves it in the
 * CR2 register. */
uint32_t paging_fault_address(void);

#endif
