/* seaweed/kernel/install.c - putting Yunix on a disk.
 *
 * An installed disk looks like this, the way GRUB itself installs on an
 * MBR disk, plus the kernel kept raw in the same gap:
 *
 *     sector 0            GRUB's boot code (bytes 0-439); the partition
 *                         table after it stays as it is
 *     sectors 1 ...       GRUB's core image: disk access, the multiboot
 *                         loader, and the two-line script
 *                         "multiboot 1024+1022" / "boot"
 *     sectors 1024 ...    the Seaweed kernel, exactly as on the ISO
 *     sector 2048 ...     the Fish partition, marked bootable
 *
 * At power-on the BIOS runs sector 0, which loads the core image from
 * sector 1. The core image reads the kernel's sectors from the same disk
 * and starts it, just as the ISO's GRUB does.
 *
 * The files come from the live ISO: its grub.cfg loads the kernel and
 * GRUB's two images as multiboot modules.
 *
 * Writing sector 0's boot code replaces whatever boot loader the disk
 * had. So the installer only touches a disk that is blank, or that holds
 * nothing but a Fish partition. Anything else is refused before a single
 * byte is written.
 */

#include "seaweed.h"
#include "block.h"
#include "partition.h"
#include "multiboot.h"
#include "heap.h"
#include "string.h"

/* Both come from the Makefile, which also builds the GRUB script that
 * loads these sectors - one source for both sides. */
#ifndef INSTALL_KERNEL_LBA
#error "INSTALL_KERNEL_LBA must come from the Makefile"
#endif

#define CORE_LBA             1
#define FIRST_PARTITION_LBA  2048
#define SECTOR               BLOCK_SECTOR_SIZE
#define CHUNK_SECTORS        64            /* 32 KiB at a time */

/* Offsets in GRUB's boot.img (grub-core/boot/i386/pc/boot.S). */
#define BOOT_CODE_BYTES      440           /* then the disk signature and table */
#define BOOT_DRIVE_CHECK     0x66          /* a 2-byte jump, see below */

#define MBR_TABLE_OFFSET     446
#define MBR_ENTRY_SIZE       16
#define MBR_ACTIVE           0x80
#define MBR_TYPE_FISH        0x7F

struct files {
    const uint8_t *kernel;  size_t kernel_size;
    const uint8_t *boot;    size_t boot_size;
    const uint8_t *core;    size_t core_size;
};

static int get_files(struct files *f)
{
    f->kernel = multiboot_module("seaweed.bin", &f->kernel_size);
    f->boot   = multiboot_module("boot.img", &f->boot_size);
    f->core   = multiboot_module("core.img", &f->core_size);
    return f->kernel != 0 && f->boot != 0 && f->core != 0;
}

int install_available(void)
{
    struct files f;
    return get_files(&f);
}

/* The files came from outside the kernel; make sure they are what they
 * claim and that they fit where they are going. */
static int files_ok(const struct files *f)
{
    if (f->kernel_size < 4 || memcmp(f->kernel, "\x7F" "ELF", 4) != 0) return 0;
    if (f->kernel_size > (size_t)INSTALL_KERNEL_SECTORS * SECTOR) return 0;
    if (f->boot_size != SECTOR) return 0;
    if (f->boot[510] != 0x55 || f->boot[511] != 0xAA) return 0;
    if (f->core_size < SECTOR) return 0;
    if (CORE_LBA + (f->core_size + SECTOR - 1) / SECTOR > INSTALL_KERNEL_LBA) return 0;
    return 1;
}

/* --- deciding --- */

static void decide(struct install_check *out)
{
    struct disk_info info;

    disk_info(&info);
    out->verdict = INSTALL_NO;
    out->keeps_files = 0;

    switch (info.layout) {
    case DISK_NONE:
        out->reason = "no such disk";
        return;

    case DISK_BLANK:
        if (info.sectors < FIRST_PARTITION_LBA + 4096) {
            out->reason = "the disk is too small (Yunix needs at least 3 MiB)";
            return;
        }
        out->verdict = INSTALL_FRESH;
        out->reason = "blank disk: a partition table, GRUB, the kernel and a Fish partition will be written";
        return;

    case DISK_FISH:
        if (strcmp(info.table, "MBR") != 0) {
            out->reason = "the installer only knows MBR disks so far (this one is GPT)";
            return;
        }
        if (info.partitions != 1) {
            out->reason = "other partitions are on this disk; GRUB's boot code would replace their boot loader";
            return;
        }
        if (info.fish_start < FIRST_PARTITION_LBA) {
            out->reason = "the Fish partition starts too early to leave room for GRUB";
            return;
        }
        out->verdict = INSTALL_KEEP_FISH;
        out->keeps_files = fish_on_disk();
        out->reason = out->keeps_files
            ? "only a Fish partition: GRUB and the kernel go before it, its files stay"
            : "only an empty Fish partition: GRUB and the kernel go before it, the files in memory go into it";
        return;

    case DISK_FOREIGN:
        out->reason = info.understood
            ? "other systems or data are on this disk"
            : "the disk holds data Yunix does not understand";
        return;
    }
}

void install_check(int disk, struct install_check *out)
{
    int previous = block_current();

    if (disk_select(disk) < 0) {
        out->verdict = INSTALL_NO;
        out->reason = "no such disk";
    } else {
        decide(out);
    }
    if (previous >= 0) disk_select(previous);
}

/* --- writing --- */

/* Writes 'size' bytes from 'data' starting at sector 'lba', padding the
 * last sector with zeros, then reads it all back and compares. */
static int write_verified(uint32_t lba, const uint8_t *data, size_t size)
{
    uint32_t sectors = (uint32_t)((size + SECTOR - 1) / SECTOR);
    uint8_t *buffer = kmalloc(CHUNK_SECTORS * SECTOR);
    int result = INSTALL_OK;

    if (buffer == 0) return INSTALL_ERR_IO;

    for (uint32_t done = 0; done < sectors && result == INSTALL_OK; ) {
        uint32_t n = sectors - done < CHUNK_SECTORS ? sectors - done : CHUNK_SECTORS;
        size_t offset = (size_t)done * SECTOR;
        size_t bytes = size - offset < (size_t)n * SECTOR ? size - offset : (size_t)n * SECTOR;

        /* Through a buffer: the module may not end on a sector, and this
         * keeps every DMA transfer in memory we own. */
        memset(buffer, 0, (size_t)n * SECTOR);
        memcpy(buffer, data + offset, bytes);
        if (block_write(lba + done, n, buffer) != 0) { result = INSTALL_ERR_IO; break; }

        memset(buffer, 0, (size_t)n * SECTOR);
        if (block_read(lba + done, n, buffer) != 0) { result = INSTALL_ERR_IO; break; }
        if (memcmp(buffer, data + offset, bytes) != 0) { result = INSTALL_ERR_VERIFY; break; }
        for (size_t i = bytes; i < (size_t)n * SECTOR; i++) {
            if (buffer[i] != 0) { result = INSTALL_ERR_VERIFY; break; }
        }
        done += n;
    }

    kfree(buffer);
    return result;
}

/* Zeros sectors [lba, lba + count): what is left of an older, bigger
 * kernel must not look like part of this one. */
static int clear_sectors(uint32_t lba, uint32_t count)
{
    uint8_t *zeros = kcalloc(CHUNK_SECTORS, SECTOR);
    int result = INSTALL_OK;

    if (zeros == 0) return INSTALL_ERR_IO;
    while (count > 0 && result == INSTALL_OK) {
        uint32_t n = count < CHUNK_SECTORS ? count : CHUNK_SECTORS;
        if (block_write(lba, n, zeros) != 0) result = INSTALL_ERR_IO;
        lba += n;
        count -= n;
    }
    kfree(zeros);
    return result;
}

/* The last step, because it is the one that makes the disk bootable:
 * GRUB's boot code into sector 0, keeping the disk signature and the
 * partition table that follow it, and the Fish partition marked active -
 * some BIOSes will not boot a disk without an active partition. */
static int write_boot_sector(const uint8_t *boot_img)
{
    uint8_t sector[SECTOR], check[SECTOR];
    int fish_entries = 0;

    if (block_read(0, 1, sector) != 0) return INSTALL_ERR_IO;

    memcpy(sector, boot_img, BOOT_CODE_BYTES);

    /* At this offset boot.img jumps past a workaround for BIOSes that
     * pass the wrong drive number. grub-install turns the jump into two
     * NOPs for hard disks, so the workaround runs; so do we. */
    sector[BOOT_DRIVE_CHECK]     = 0x90;
    sector[BOOT_DRIVE_CHECK + 1] = 0x90;

    for (int i = 0; i < 4; i++) {
        uint8_t *entry = sector + MBR_TABLE_OFFSET + i * MBR_ENTRY_SIZE;
        if (entry[4] == MBR_TYPE_FISH) { entry[0] = MBR_ACTIVE; fish_entries++; }
        else                             entry[0] = 0;
    }
    if (fish_entries != 1) return INSTALL_ERR_REFUSED;    /* checked before, but be sure */

    sector[510] = 0x55;
    sector[511] = 0xAA;

    if (block_write(0, 1, sector) != 0) return INSTALL_ERR_IO;
    if (block_read(0, 1, check) != 0) return INSTALL_ERR_IO;
    return memcmp(sector, check, SECTOR) == 0 ? INSTALL_OK : INSTALL_ERR_VERIFY;
}

int install_run(int disk)
{
    struct files f;
    struct install_check verdict;
    uint32_t kernel_sectors;
    int result;

    if (!get_files(&f)) return INSTALL_ERR_NOT_LIVE;
    if (!files_ok(&f))  return INSTALL_ERR_FILES;

    /* Fish moves to this disk; everything below works on it. */
    if (disk_select(disk) < 0) return INSTALL_ERR_REFUSED;
    decide(&verdict);
    if (verdict.verdict == INSTALL_NO) return INSTALL_ERR_REFUSED;

    if (verdict.verdict == INSTALL_FRESH && partition_create_fish() != 0) return INSTALL_ERR_IO;

    /* The kernel first, then GRUB's core, and the boot sector last: until
     * that one is written, the disk does not try to boot a half-written
     * install. */
    kernel_sectors = (uint32_t)((f.kernel_size + SECTOR - 1) / SECTOR);
    result = write_verified(INSTALL_KERNEL_LBA, f.kernel, f.kernel_size);
    if (result == INSTALL_OK) {
        result = clear_sectors(INSTALL_KERNEL_LBA + kernel_sectors,
                               INSTALL_KERNEL_SECTORS - kernel_sectors);
    }
    if (result == INSTALL_OK) result = write_verified(CORE_LBA, f.core, f.core_size);
    if (result == INSTALL_OK) result = write_boot_sector(f.boot);
    if (result != INSTALL_OK) return result;

    /* Fish: a partition that already holds files keeps them, and those
     * become the files in memory. Otherwise the files in memory - the
     * ones made during the live session - are saved into it. */
    if (fish_on_disk()) {
        fish_init();
        if (fish_load() != FISH_OK) return INSTALL_ERR_FISH;
    } else {
        if (fish_save() != FISH_OK) return INSTALL_ERR_FISH;
    }
    return INSTALL_OK;
}

const char *install_error(int code)
{
    switch (code) {
    case INSTALL_OK:           return "ok";
    case INSTALL_ERR_NOT_LIVE: return "start Yunix from the live ISO (CD or USB) to install it";
    case INSTALL_ERR_FILES:    return "the files from the ISO do not look right";
    case INSTALL_ERR_REFUSED:  return "Yunix will not install on this disk";
    case INSTALL_ERR_IO:       return "disk error";
    case INSTALL_ERR_VERIFY:   return "what was written did not read back the same";
    case INSTALL_ERR_FISH:     return "GRUB and the kernel are in, but saving the files failed";
    default:                   return "unknown error";
    }
}
