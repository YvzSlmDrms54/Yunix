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
#include "keyboard.h"
#include "timer.h"
#include "ata.h"
#include "heap.h"

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
        vga_write("[ ok ] ATA disk found on the primary channel\n");

        if (fish_load() == FISH_OK) {
            vga_write("[ ok ] Fish filesystem restored from disk\n");
        } else {
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            vga_write("[ .. ] No Fish image on the disk, starting empty\n");
        }
    } else {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_write("[ .. ] No disk found. Files will vanish on reboot.\n");
    }
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    yunix_main();   /* never returns */

    power_halt();   /* in case it ever does */
}
