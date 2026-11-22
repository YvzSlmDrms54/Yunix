/* seaweed/kernel/kernel.c - the Seaweed kernel's entry point.
 *
 * Brings the hardware up, loads the filesystem, then hands the machine
 * to Yunix through yunix_main(). Nothing in here knows about the shell,
 * users or commands - that is all Yunix.
 */

#include "seaweed.h"
#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "paging.h"
#include "keyboard.h"
#include "timer.h"
#include "ata.h"
#include "heap.h"
#include "partition.h"
#include "string.h"

/* Prints a sector count as whole MiB (2048 sectors of 512 bytes each). */
static void write_mib(uint32_t sectors)
{
    char buffer[12];

    utoa(sectors / 2048, buffer, 10);
    vga_write(buffer);
    vga_write(" MiB");
}

void kernel_main(void)
{
    vga_init();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_write("Seaweed v" SEAWEED_VERSION "\n\n");

    gdt_init();
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("[ ok ] GDT loaded\n");

    idt_init();
    vga_write("[ ok ] IDT loaded, PIC remapped to 32-47\n");

    paging_init();
    vga_write("[ ok ] Paging on: 16 MiB identity-mapped, page 0 left out\n");

    timer_init();
    vga_write("[ ok ] PIT running at 100 Hz on IRQ 0\n");

    keyboard_init();
    vga_write("[ ok ] Keyboard driver installed on IRQ 1\n");

    __asm__ volatile ("sti");
    vga_write("[ ok ] Interrupts enabled\n");

    heap_init();
    vga_write("[ ok ] Heap ready (1 MiB)\n");

    fish_init();
    if (ata_init()) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_write("[ ok ] ATA disk found on the primary channel (");
        write_mib(ata_sectors());
        vga_write(")\n");

        switch (partition_scan()) {
        case DISK_FISH:
            vga_write("[ ok ] Fish partition found\n");
            if (fish_load() == FISH_OK) {
                vga_write("[ ok ] Fish filesystem restored from disk\n");
            } else {
                vga_set_color(VGA_YELLOW, VGA_BLACK);
                vga_write("[ .. ] Fish partition holds no image yet, starting empty\n");
            }
            break;

        case DISK_BLANK:
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            vga_write("[ .. ] Disk is blank: no partitions, no Fish yet\n");
            break;

        default:
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            vga_write("[ .. ] Disk holds other data and no Fish partition.\n");
            vga_write("       Seaweed will not write to it.\n");
            break;
        }
    } else {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_write("[ .. ] No disk found. Files will vanish on reboot.\n");
    }
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    yunix_main();   /* never returns */

    power_halt();   /* in case it ever does */
}
