#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* Selectors: the byte offset of each entry inside the GDT.
 * Entry 0 is at offset 0, entry 1 at offset 8, entry 2 at 16, and so on.
 * These are the values you load into segment registers. */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

/* GDT entries 6-8 are for thread-local storage, as on Linux. */
#define GDT_TLS_FIRST   6
#define GDT_TLS_COUNT   3

/* A selector's low two bits are the privilege level it asks for, so
 * ring 3 loads the user selectors with 3 added: 0x1B and 0x23. */
#define GDT_USER_CODE_RPL3 (GDT_USER_CODE | 3)
#define GDT_USER_DATA_RPL3 (GDT_USER_DATA | 3)

/* One GDT entry, exactly 8 bytes.
 *
 * The layout is a mess for historical reasons: the base address and the
 * limit are split into pieces scattered across the structure, because the
 * 286 had 24-bit addresses and the 386 had to stay compatible.
 *
 * __attribute__((packed)) forbids the compiler from inserting padding.
 * Without it the CPU would read garbage - this is not optional. */
struct gdt_entry {
    uint16_t limit_low;    /* limit, bits 0-15  */
    uint16_t base_low;     /* base,  bits 0-15  */
    uint8_t  base_middle;  /* base,  bits 16-23 */
    uint8_t  access;       /* permissions and type */
    uint8_t  granularity;  /* limit bits 16-19 + flags */
    uint8_t  base_high;    /* base,  bits 24-31 */
} __attribute__((packed));

/* What we hand to the CPU: the size of the table and where it lives. */
struct gdt_pointer {
    uint16_t limit;   /* size of the table in bytes, minus one */
    uint32_t base;    /* address of the first entry */
} __attribute__((packed));

void gdt_init(void);

/* Sets or clears one thread-local storage entry. Returns -1 for an index
 * outside entries 6-8. */
int  gdt_set_tls(int index, uint32_t base, uint32_t limit, int limit_in_pages, int present);
void gdt_clear_tls(void);

/* The kernel stack the CPU switches to when ring 3 is interrupted. */
void tss_set_kernel_stack(uint32_t esp0);

#endif
