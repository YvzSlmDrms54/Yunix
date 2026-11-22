/* seaweed/kernel/gdt.c - Global Descriptor Table.
 *
 * We use a "flat" model: every segment starts at address 0 and covers the
 * whole 4 GiB address space. That means segmentation effectively does
 * nothing, which is what we want - x86 forces us to have a GDT, but modern
 * systems do memory protection with paging instead.
 *
 * We still need separate code and data entries, and separate kernel and
 * user entries, because the CPU checks the type and privilege bits.
 */

#include "gdt.h"

#define GDT_ENTRIES 6

static struct gdt_entry   gdt[GDT_ENTRIES];
static struct gdt_pointer gdt_ptr;

/* The Task State Segment. x86 once used it for hardware task switching,
 * which nobody does any more; what is left that matters is one job: when
 * an interrupt or a system call arrives while a ring 3 program runs, the
 * CPU must switch to a kernel stack - the program's own stack cannot be
 * trusted - and it takes that stack's address from here (ss0:esp0). */
struct tss {
    uint32_t prev_task;
    uint32_t esp0, ss0;           /* the kernel stack for ring 3 -> ring 0 */
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

static struct tss tss;

void tss_set_kernel_stack(uint32_t esp0)
{
    tss.esp0 = esp0;
}

/* Implemented in boot/gdt_flush.s */
extern void gdt_flush(uint32_t gdt_ptr_address);

/* Fills one entry, hiding the ugly bit-splitting.
 *
 * access byte:
 *   bit 7  present        - 1 means this entry is valid
 *   bit 6-5 privilege     - 0 = kernel (ring 0), 3 = user (ring 3)
 *   bit 4  type           - 1 for code/data, 0 for system entries
 *   bit 3  executable     - 1 = code segment, 0 = data segment
 *   bit 2  direction      - 0 for our purposes
 *   bit 1  read/write     - code: readable, data: writable
 *   bit 0  accessed       - the CPU sets this itself
 *
 * granularity byte:
 *   bit 7  granularity    - 1 means the limit counts 4 KiB pages, not bytes
 *   bit 6  size           - 1 = 32-bit protected mode
 *   bit 5-4 unused here
 *   bit 3-0 limit bits 16-19
 */
static void gdt_set_entry(int index, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags)
{
    gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);

    gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[index].granularity = (uint8_t)((limit >> 16) & 0x0F);
    gdt[index].granularity |= (uint8_t)(flags & 0xF0);

    gdt[index].access      = access;
}

void gdt_init(void)
{
    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base  = (uint32_t)&gdt;

    /* Entry 0 must be all zeros. The CPU refuses to use it, which catches
     * bugs where a segment register was never initialised. */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* limit 0xFFFFF with 4 KiB granularity = 0xFFFFF * 4096 = 4 GiB */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xC0);  /* kernel code, ring 0 */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);  /* kernel data, ring 0 */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xC0);  /* user code,   ring 3 */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);  /* user data,   ring 3 */

    /* The TSS is a system entry: 0x89 = present, ring 0, "32-bit TSS,
     * not busy". Its limit counts bytes, so no granularity flag. An I/O
     * map offset past the end means no I/O ports for ring 3 at all. */
    tss.ss0 = GDT_KERNEL_DATA;
    tss.iomap_base = sizeof(tss);
    gdt_set_entry(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);

    gdt_flush((uint32_t)&gdt_ptr);

    /* Tell the CPU which GDT entry is the TSS. */
    __asm__ volatile ("ltr %0" : : "r"((uint16_t)GDT_TSS));
}