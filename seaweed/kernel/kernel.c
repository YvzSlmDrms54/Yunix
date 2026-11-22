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
#include "pci.h"
#include "ahci.h"
#include "block.h"
#include "string.h"

/* Prints a sector count as whole MiB (2048 sectors of 512 bytes each). */
static void write_mib(uint32_t sectors)
{
    char buffer[12];

    utoa(sectors / 2048, buffer, 10);
    vga_write(buffer);
    vga_write(" MiB");
}

/* Two hex digits, as in a PCI address like 00:1f.2. */
static void write_hex2(uint8_t value)
{
    static const char digits[] = "0123456789abcdef";

    vga_putchar(digits[value >> 4]);
    vga_putchar(digits[value & 0xF]);
}

/* One boot line: how many PCI devices there are, and where the AHCI
 * controller is, since that is the one the next disk driver will need. */
static void report_pci(void)
{
    const struct pci_device *ahci = pci_find(0x01, 0x06, 0x01);
    char buffer[12];

    vga_write("[ ok ] PCI: ");
    utoa((unsigned int)pci_found(), buffer, 10);
    vga_write(buffer);
    vga_write(pci_found() == 1 ? " device" : " devices");

    if (ahci != 0) {
        vga_write(", AHCI controller at ");
        write_hex2(ahci->bus);
        vga_putchar(':');
        write_hex2(ahci->slot);
        vga_putchar('.');
        vga_putchar((char)('0' + ahci->func));
    }
    vga_putchar('\n');
}

/* One boot line about the SATA controller, if there is one: its AHCI
 * version, then how many disks and CD drives hang off it. */
static void report_ahci(int found)
{
    uint32_t version = ahci_version();
    int disks = 0, drives = 0;
    char buffer[12];

    if (found < 0) {
        if (pci_find(0x01, 0x06, 0x01) != 0) {
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            vga_write("[ .. ] AHCI controller found, but it could not be reached\n");
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        }
        return;
    }

    for (int i = 0; i < ahci_count(); i++) {
        if (ahci_get(i)->kind == AHCI_DISK)  disks++;
        if (ahci_get(i)->kind == AHCI_ATAPI) drives++;
    }

    vga_write("[ ok ] AHCI ");
    utoa(version >> 16, buffer, 10);
    vga_write(buffer);
    vga_putchar('.');
    utoa((version >> 8) & 0xFF, buffer, 10);
    vga_write(buffer);
    if (version & 0xFF) {
        vga_putchar('.');
        utoa(version & 0xFF, buffer, 10);
        vga_write(buffer);
    }
    vga_write(": ");
    utoa((unsigned int)disks, buffer, 10);
    vga_write(buffer);
    vga_write(disks == 1 ? " SATA disk" : " SATA disks");
    if (drives > 0) {
        vga_write(", ");
        utoa((unsigned int)drives, buffer, 10);
        vga_write(buffer);
        vga_write(drives == 1 ? " CD/DVD drive" : " CD/DVD drives");
    }
    vga_putchar('\n');
}

/* Which disk Fish should live on: the first one that already has a
 * Fish partition, so a machine with Yunix installed finds its files no
 * matter which port the disk is on. Without one, the first disk - where
 * format yes or partition new can make one. */
static int choose_disk(void)
{
    for (int i = 0; i < block_count(); i++) {
        if (disk_select(i) == DISK_FISH) return i;
    }
    return 0;
}

static void write_disk_name(int index)
{
    struct block_info info;
    char buffer[12];

    if (block_get(index, &info) != 0) return;

    if (info.kind == BLOCK_IDE) {
        vga_write("IDE primary master");
    } else {
        vga_write("SATA port ");
        utoa((unsigned int)info.port, buffer, 10);
        vga_write(buffer);
        vga_write(", ");
        vga_write(info.model);
    }
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

    pci_init();
    report_pci();
    report_ahci(ahci_init());

    fish_init();
    if (ata_init()) {
        vga_write("[ ok ] IDE disk on the primary channel (");
        write_mib(ata_sectors());
        vga_write(")\n");
    }

    block_init();
    if (block_count() > 0) {
        int chosen = choose_disk();

        vga_write("[ ok ] Fish uses disk ");
        vga_putchar((char)('1' + chosen));
        vga_write(": ");
        write_disk_name(chosen);
        vga_putchar('\n');

        switch ((enum disk_layout)disk_select(chosen)) {
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
        vga_write("[ .. ] No disk for Fish. Files will vanish on reboot.\n");
    }
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    yunix_main();   /* never returns */

    power_halt();   /* in case it ever does */
}
