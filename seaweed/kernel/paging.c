/* seaweed/kernel/paging.c - turning on paging.
 *
 * With paging on, every address the CPU uses is looked up in tables the
 * kernel builds. On 32-bit x86 a virtual address splits into three parts:
 *
 *     | 10 bits         | 10 bits     | 12 bits        |
 *     | directory index | table index | offset in page |
 *
 * The page directory (1024 entries) points to page tables (1024 entries
 * each), and each page table entry points to one 4 KiB page of physical
 * memory. CR3 holds the physical address of the directory, and bit 31 of
 * CR0 switches the whole thing on.
 *
 * For now every mapped page points to itself ("identity mapping"), so the
 * kernel keeps running at the same addresses after the switch. The one
 * change is page 0: it is not mapped, so a NULL pointer causes a page
 * fault instead of silently touching the first 4 KiB of memory.
 */

#include "paging.h"

#define PAGE_SIZE        4096
#define ENTRIES          1024
#define TABLE_COUNT      (PAGING_MAPPED_MB / 4)    /* one table covers 4 MiB */

/* Bits shared by directory and table entries. */
#define PAGE_PRESENT     0x001    /* the entry is valid */
#define PAGE_WRITABLE    0x002    /* writes are allowed */
/* Bit 2 (user) stays clear: only ring 0 - the kernel - may use these
 * pages. Nothing runs in ring 3 yet, but the protection is already right
 * for when something does. */

#define CR0_WRITE_PROTECT (1u << 16)  /* enforce read-only pages in ring 0 too */
#define CR0_PAGING        (1u << 31)

/* The CPU wants both the directory and every table on a 4 KiB boundary,
 * because the low 12 bits of an entry are flags, not address. */
static uint32_t page_directory[ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint32_t page_tables[TABLE_COUNT][ENTRIES] __attribute__((aligned(PAGE_SIZE)));

void paging_init(void)
{
    uint32_t cr0;

    for (uint32_t t = 0; t < TABLE_COUNT; t++) {
        for (uint32_t i = 0; i < ENTRIES; i++) {
            uint32_t address = (t * ENTRIES + i) * PAGE_SIZE;
            page_tables[t][i] = address | PAGE_PRESENT | PAGE_WRITABLE;
        }
    }

    /* Page 0 stays unmapped: that is what makes NULL pointers fault. */
    page_tables[0][0] = 0;

    for (uint32_t d = 0; d < ENTRIES; d++) {
        if (d < TABLE_COUNT) {
            page_directory[d] = (uint32_t)page_tables[d] | PAGE_PRESENT | PAGE_WRITABLE;
        } else {
            page_directory[d] = 0;          /* nothing above 16 MiB */
        }
    }

    /* The addresses above are physical as well as virtual - that is only
     * true because we are identity mapping, and because the kernel itself
     * runs at the address it was loaded to. */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory) : "memory");

    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_PAGING | CR0_WRITE_PROTECT;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

uint32_t paging_fault_address(void)
{
    uint32_t address;

    __asm__ volatile ("mov %%cr2, %0" : "=r"(address));
    return address;
}
