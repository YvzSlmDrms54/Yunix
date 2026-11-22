/* yunix/shell.c - the Yunix command shell. */

#include "shell.h"
#include "seaweed.h"
#include "yunix.h"
#include "string.h"
#include "user.h"
#include "apps.h"

#define LINE_MAX 256

static char line[LINE_MAX];

/* --- commands --- */

/* Defined at the bottom: it walks the command table, which does not
 * exist yet at this point in the file. */
static int cmd_help(const char *arg);

static int cmd_clear(const char *arg)
{
    (void)arg;
    vga_clear();

    return 0;
}

static int cmd_echo(const char *arg)
{
    vga_write(arg);
    vga_putchar('\n');

    return 0;
}

static int cmd_neofetch(const char *arg)
{
    (void)arg;
    vga_write("\xDB\xDB\xBB   \xDB\xDB\xBB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xBB\n");
    vga_write("\xC8\xDB\xDB\xBB \xDB\xDB\xC9\xBC\xDB\xDB\xC9\xCD\xCD\xCD\xCD\xBC\n");
    vga_write(" \xC8\xDB\xDB\xDB\xDB\xC9\xBC \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xBB\n");
    vga_write("  \xC8\xDB\xDB\xC9\xBC  \xC8\xCD\xCD\xCD\xCD\xDB\xDB\xBA\n");
    vga_write("   \xDB\xDB\xBA   \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xBA\n");
    vga_write("   \xC8\xCD\xBC   \xC8\xCD\xCD\xCD\xCD\xCD\xCD\xBC\n");

    return 0;
}

static int cmd_about(const char *arg)
{
    (void)arg;
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_write("Yunix v" YUNIX_VERSION "\n");
    vga_write("Kernel: Seaweed v" SEAWEED_VERSION "\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_write("A hobby operating system by Yavuz Selim.\n");
    vga_write("32-bit x86, booted by GRUB via multiboot.\n");
    vga_write("Written in C and assembly, no libc underneath.\n");

    return 0;
}

/* Prints a number followed by a word, adding "s" when it is not one. */
static void print_unit(uint32_t value, const char *word)
{
    char buffer[12];

    utoa(value, buffer, 10);
    vga_write(buffer);
    vga_putchar(' ');
    vga_write(word);
    if (value != 1) vga_putchar('s');
}

static int cmd_uptime(const char *arg)
{
    uint32_t total = timer_seconds();
    uint32_t hours = total / 3600;
    uint32_t minutes = (total / 60) % 60;
    uint32_t seconds = total % 60;

    (void)arg;

    vga_write("up ");
    if (hours > 0)   { print_unit(hours, "hour");     vga_write(", "); }
    if (minutes > 0) { print_unit(minutes, "minute"); vga_write(", "); }
    print_unit(seconds, "second");
    vga_putchar('\n');

    return 0;
}

static int cmd_ticks(const char *arg)
{
    char buffer[12];

    (void)arg;

    utoa(timer_ticks(), buffer, 10);
    vga_write(buffer);
    vga_write(" ticks at ");
    utoa(TIMER_HZ, buffer, 10);
    vga_write(buffer);
    vga_write(" Hz\n");

    return 0;
}

static int cmd_sleep(const char *arg)
{
    uint32_t seconds = 0;

    if (arg[0] == '\0') { vga_write("Usage: sleep <seconds>\n"); return 1; }

    for (size_t i = 0; arg[i] != '\0'; i++) {
        if (arg[i] < '0' || arg[i] > '9') {
            vga_write("sleep: not a number\n");
            return 1;
        }
        seconds = seconds * 10 + (uint32_t)(arg[i] - '0');
    }

    if (seconds > 60) { vga_write("sleep: 60 seconds maximum\n"); return 1; }

    timer_sleep(seconds * 1000);

    return 0;
}

static int cmd_whoami(const char *arg)
{
    (void)arg;
    vga_write(user_name());
    vga_putchar('\n');

    return 0;
}

static int cmd_user(const char *arg)
{
    if (arg[0] == '\0') {
        vga_write("Usage: user <name>\n");
        return 1;
    }
    if (!user_set_name(arg)) {
        vga_write("user: names cannot be empty or contain spaces\n");
        return 1;
    }
    user_save();

    return 0;
}

static int cmd_hostname(const char *arg)
{
    if (arg[0] == '\0') {
        vga_write(user_host());
        vga_putchar('\n');
        return 0;
    }
    if (!user_set_host(arg)) {
        vga_write("hostname: names cannot be empty or contain spaces\n");
        return 1;
    }
    user_save();

    return 0;
}

/* ---- Fish filesystem commands ---- */

/* Prints "cmd: what went wrong" and returns 1, so a command can end
 * with "return fish_fail(...)". */
static int fish_fail(const char *cmd, int code)
{
    vga_write(cmd);
    vga_write(": ");
    vga_write(fish_error(code));
    vga_putchar('\n');
    return 1;
}

static int cmd_pwd(const char *arg)
{
    char path[FISH_PATH_MAX];

    (void)arg;
    fish_path(path, sizeof(path));
    vga_write(path);
    vga_putchar('\n');

    return 0;
}

static int cmd_ls(const char *arg)
{
    int handle = fish_first();
    const char *name;
    int is_dir;
    size_t size;
    int count = 0;
    char buffer[12];

    (void)arg;

    while ((handle = fish_next(handle, &name, &is_dir, &size)) != 0) {
        if (is_dir) {
            vga_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
            vga_write("[DIR]  ");
        } else {
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            vga_write("[FILE] ");
        }

        vga_write(name);

        if (!is_dir) {
            vga_write("  (");
            utoa((unsigned int)size, buffer, 10);
            vga_write(buffer);
            vga_write(" bytes)");
        }

        vga_putchar('\n');
        count++;
    }

    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    if (count == 0) vga_write("Directory is empty.\n");

    return 0;
}

static int cmd_cd(const char *arg)
{
    int result;

    if (arg[0] == '\0') { fish_chdir("/"); return 0; }

    result = fish_chdir(arg);
    if (result < 0) return fish_fail("cd", result);

    return 0;
}

static int cmd_mkdir(const char *arg)
{
    int result;

    if (arg[0] == '\0') { vga_write("Usage: mkdir <name>\n"); return 1; }

    result = fish_mkdir(arg);
    if (result < 0) return fish_fail("mkdir", result);

    return 0;
}

static int cmd_touch(const char *arg)
{
    int result;

    if (arg[0] == '\0') { vga_write("Usage: touch <name>\n"); return 1; }

    result = fish_create(arg);
    if (result < 0) return fish_fail("touch", result);

    return 0;
}

static int cmd_rm(const char *arg)
{
    int result;

    if (arg[0] == '\0') { vga_write("Usage: rm <name>\n"); return 1; }

    result = fish_remove(arg);
    if (result < 0) return fish_fail("rm", result);

    return 0;
}

static int cmd_cat(const char *arg)
{
    size_t size;
    const char *data;

    if (arg[0] == '\0') { vga_write("Usage: cat <file>\n"); return 1; }

    data = fish_read(arg, &size);
    if (data == 0) { vga_write("cat: no such file\n"); return 1; }

    for (size_t i = 0; i < size; i++) vga_putchar(data[i]);
    if (size > 0 && data[size - 1] != '\n') vga_putchar('\n');

    return 0;
}

/* write <file> <text>  - replaces the contents
 * append <file> <text> - adds to the end
 *
 * Both need to split their argument again: the shell only splits off the
 * command name, so 'arg' still holds "file text here". */
static int write_common(const char *arg, int appending)
{
    char name[FISH_NAME_MAX];
    size_t i = 0;
    const char *text;
    int result;

    if (arg[0] == '\0') {
        vga_write(appending ? "Usage: append <file> <text>\n"
                            : "Usage: write <file> <text>\n");
        return 1;
    }

    while (arg[i] != '\0' && arg[i] != ' ' && i < FISH_NAME_MAX - 1) {
        name[i] = arg[i];
        i++;
    }
    name[i] = '\0';

    text = arg + i;
    while (*text == ' ') text++;

    result = appending ? fish_append(name, text) : fish_write(name, text);
    if (result < 0) return fish_fail(appending ? "append" : "write", result);

    return 0;
}

static int cmd_yvim(const char *arg) { return yvim_run(arg); }

static int cmd_write(const char *arg)  { return write_common(arg, 0); }
static int cmd_append(const char *arg) { return write_common(arg, 1); }

static int cmd_save(const char *arg)
{
    int result;

    (void)arg;

    user_save();

    result = fish_save();
    if (result < 0) { fish_fail("save", result); return 1; }
    vga_write("Filesystem written to disk.\n");

    return 0;
}

static int cmd_load(const char *arg)
{
    int result;

    (void)arg;

    result = fish_load();
    if (result < 0) { fish_fail("load", result); return 1; }
    vga_write("Filesystem restored from disk.\n");

    return 0;
}

/* Prints a sector count as MiB. */
static void write_mib(uint32_t sectors)
{
    char buffer[12];

    utoa(sectors / 2048, buffer, 10);
    vga_write(buffer);
    vga_write(" MiB");
}

static int cmd_format(const char *arg)
{
    struct disk_info info;
    int result;

    disk_info(&info);

    if (strcmp(arg, "yes") != 0) {
        if (info.layout == DISK_BLANK) {
            vga_write("The disk is blank. This creates a partition table with\n");
            vga_write("one Fish partition on it, and erases every file in memory.\n");
        } else {
            vga_write("This erases every file, in memory and in the Fish partition.\n");
        }
        vga_write("Type: format yes\n");
        return 1;
    }

    /* fish_format checks everything before it wipes anything: on a disk
     * that belongs to something else it refuses and changes nothing. */
    result = fish_format();
    if (result < 0) return fish_fail("format", result);

    if (info.layout == DISK_BLANK) {
        vga_write("Created a partition table with one Fish partition.\n");
    }
    vga_write("Filesystem erased and written to disk.\n");
    return 0;
}

static int parse_number(const char *text, uint32_t *out);

/* "IDE primary master, 16 MiB" or "SATA port 0, QEMU HARDDISK, 32 MiB". */
static void write_disk(const struct block_info *b)
{
    char buffer[12];

    if (b->kind == BLOCK_IDE) {
        vga_write("IDE primary master");
    } else {
        vga_write("SATA port ");
        utoa((unsigned int)b->port, buffer, 10);
        vga_write(buffer);
        vga_write(", ");
        vga_write(b->model);
    }
    vga_write(", ");
    /* Shifting, not dividing: 64-bit division would need a helper
     * library we do not have. */
    utoa((unsigned int)(b->sectors >> 11), buffer, 10);
    vga_write(buffer);
    vga_write(" MiB");
}

static const char *layout_name(int layout)
{
    switch (layout) {
    case DISK_FISH:  return "Fish partition";
    case DISK_BLANK: return "blank";
    default:         return "other data, no Fish";
    }
}

/* One line per disk, with what is on it. Looking at each disk means
 * selecting it for a moment; the disk Fish uses is selected again after. */
static void list_disks(void)
{
    int in_use = block_current();
    char buffer[12];

    vga_write("Disks:\n");
    for (int i = 0; i < block_count(); i++) {
        struct block_info b;
        struct disk_info info;
        int layout;

        block_get(i, &b);
        layout = disk_select(i);
        disk_info(&info);

        vga_write(i == in_use ? "* " : "  ");
        utoa((unsigned int)i + 1, buffer, 10);
        vga_write(buffer);
        vga_write(". ");
        write_disk(&b);
        vga_write(": ");
        if (layout != DISK_BLANK) { vga_write(info.table); vga_write(", "); }
        vga_write(layout_name(layout));
        vga_putchar('\n');
    }
    if (in_use >= 0) disk_select(in_use);

    for (int i = 0; i < ahci_count(); i++) {
        const struct ahci_device *d = ahci_get(i);

        if (d->kind != AHCI_ATAPI) continue;
        vga_write("CD/DVD: SATA port ");
        utoa((unsigned int)d->port, buffer, 10);
        vga_write(buffer);
        if (d->model[0] != '\0') { vga_write(", "); vga_write(d->model); }
        vga_putchar('\n');
    }
}

/* The disk Fish is on, in detail. */
static void describe_fish_disk(void)
{
    struct disk_info info;
    char buffer[12];

    disk_info(&info);

    vga_write("Fish is on disk ");
    utoa((unsigned int)block_current() + 1, buffer, 10);
    vga_write(buffer);
    vga_write(" (");
    write_mib(info.sectors);
    vga_write(" usable). Partition table: ");
    vga_write(info.table);
    if (info.partitions > 0) {
        vga_write(", ");
        utoa((unsigned int)info.partitions, buffer, 10);
        vga_write(buffer);
        vga_write(info.partitions == 1 ? " partition" : " partitions");
    }
    vga_putchar('\n');

    switch (info.layout) {
    case DISK_FISH:
        vga_write("Fish partition: from sector ");
        utoa(info.fish_start, buffer, 10);
        vga_write(buffer);
        vga_write(", ");
        write_mib(info.fish_sectors);
        vga_putchar('\n');
        break;
    case DISK_BLANK:
        vga_write("The disk is blank. format yes will make a Fish partition.\n");
        break;
    default:
        vga_write("No Fish partition. Yunix will not write to this disk.\n");
        vga_write("Type partition to look for free space for one.\n");
        break;
    }
}

/* disk              - list the disks and show the one Fish uses
 * disk <n> [yes]    - move Fish to disk n, loading its files */
static int cmd_disk(const char *arg)
{
    char number[12];
    size_t i = 0, n = 0;
    uint32_t which;
    int confirmed, layout, result;

    /* Fails when there is no disk for Fish, so "disk && save" does what
     * it says. */
    if (block_count() == 0) {
        vga_write("No disks found.\n");
        list_disks();
        return 1;
    }

    if (arg[0] == '\0') {
        list_disks();
        describe_fish_disk();
        if (block_count() > 1) vga_write("Type disk <number> to move Fish to another disk.\n");
        return 0;
    }

    while (arg[i] != '\0' && arg[i] != ' ' && n < sizeof(number) - 1) number[n++] = arg[i++];
    number[n] = '\0';
    while (arg[i] == ' ') i++;
    confirmed = strcmp(arg + i, "yes") == 0;

    if (!parse_number(number, &which) || which == 0 || which > (uint32_t)block_count() ||
        (arg[i] != '\0' && !confirmed)) {
        vga_write("Usage: disk [number [yes]]\n");
        return 1;
    }

    if ((int)which - 1 == block_current()) {
        vga_write("Fish is already on that disk.\n");
        return 0;
    }

    /* Moving loads the other disk's files in place of these ones. If
     * these have changes the disk does not have, they would be gone. */
    if (fish_unsaved() && !confirmed) {
        vga_write("The files in memory have changes that are not saved.\n");
        vga_write("Type save first, or: disk ");
        vga_write(number);
        vga_write(" yes   to drop them.\n");
        return 1;
    }

    layout = disk_select((int)which - 1);
    fish_init();
    user_init();

    if (layout == DISK_FISH) {
        result = fish_load();
        if (result == FISH_OK) {
            user_load();
            vga_write("Fish moved to disk ");
            vga_write(number);
            vga_write(", files loaded from it.\n");
        } else {
            vga_write("Fish moved to disk ");
            vga_write(number);
            vga_write(". Its Fish partition holds no image yet: starting empty.\n");
        }
    } else {
        vga_write("Fish moved to disk ");
        vga_write(number);
        vga_write(" (");
        vga_write(layout_name(layout));
        vga_write("), starting empty.\n");
    }
    return 0;
}

/* Reads a whole decimal number. Returns 0 if 'text' is not one, or does
 * not fit in 32 bits. */
static int parse_number(const char *text, uint32_t *out)
{
    uint32_t value = 0;

    if (text[0] == '\0') return 0;
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9') return 0;
        if (value > (0xFFFFFFFFu - 9) / 10) return 0;
        value = value * 10 + (uint32_t)(text[i] - '0');
    }
    *out = value;
    return 1;
}

#define PARTITION_LIST_MAX 32

static void write_number(uint32_t value)
{
    char buffer[12];

    utoa(value, buffer, 10);
    vga_write(buffer);
}

static void list_partitions(void)
{
    struct disk_info info;
    struct disk_partition parts[PARTITION_LIST_MAX];
    struct disk_region free_space[PARTITION_LIST_MAX];
    int part_count, free_count;

    disk_info(&info);

    vga_write("Disk: ");
    write_mib(info.sectors);
    vga_write(", partition table: ");
    vga_write(info.table);
    vga_putchar('\n');

    part_count = disk_partitions(parts, PARTITION_LIST_MAX);
    if (part_count == 0) vga_write("Partitions: none\n");
    else                 vga_write("Partitions:\n");
    for (int i = 0; i < part_count; i++) {
        vga_write("   from sector ");
        write_number(parts[i].start);
        vga_write(", ");
        write_mib(parts[i].sectors);
        vga_write(", ");
        vga_write(parts[i].kind);
        vga_putchar('\n');
    }

    if (!info.understood) {
        vga_write("Yunix cannot make sense of this table, so it will not change it.\n");
        return;
    }

    free_count = disk_free_space(free_space, PARTITION_LIST_MAX);
    if (free_count == 0) {
        vga_write("Free space: none (a Fish partition needs at least 1 MiB)\n");
        return;
    }

    vga_write("Free space:\n");
    for (int i = 0; i < free_count; i++) {
        vga_write("  ");
        write_number((uint32_t)i + 1);
        vga_write(". from sector ");
        write_number(free_space[i].start);
        vga_write(", ");
        write_mib(free_space[i].sectors);
        vga_putchar('\n');
    }

    if (info.layout != DISK_FISH) {
        vga_write("To use one: partition new <number> [size in MiB]\n");
    }
}

/* partition                       - list partitions and free space
 * partition new <n> [MiB] [yes]   - make a Fish partition in free space n */
static int cmd_partition(const char *arg)
{
    struct disk_region free_space[PARTITION_LIST_MAX];
    char words[4][16];
    int word_count = 0;
    uint32_t which, size_mib = 0, sectors;
    int confirmed = 0, free_count, result;
    size_t i = 0;

    if (block_current() < 0) { vga_write("partition: no disk found\n"); return 1; }

    /* Split the argument into words. */
    while (arg[i] != '\0') {
        size_t n = 0;

        while (arg[i] == ' ') i++;
        if (arg[i] == '\0') break;
        if (word_count == 4) { vga_write("partition: too many words\n"); return 1; }
        while (arg[i] != '\0' && arg[i] != ' ') {
            if (n < sizeof(words[0]) - 1) words[word_count][n++] = arg[i];
            i++;
        }
        words[word_count++][n] = '\0';
    }

    if (word_count == 0) { list_partitions(); return 0; }

    if (strcmp(words[0], "new") != 0 || word_count < 2) {
        vga_write("Usage: partition                      list partitions and free space\n");
        vga_write("       partition new <n> [MiB] [yes]  make a Fish partition there\n");
        return 1;
    }

    if (!parse_number(words[1], &which) || which == 0) {
        vga_write("partition: pick a free space number from the list\n");
        return 1;
    }
    for (int w = 2; w < word_count; w++) {
        if (strcmp(words[w], "yes") == 0 && w == word_count - 1) confirmed = 1;
        else if (w == 2 && parse_number(words[w], &size_mib) && size_mib > 0) { }
        else { vga_write("partition: the size must be a number of MiB\n"); return 1; }
    }

    free_count = disk_free_space(free_space, PARTITION_LIST_MAX);
    if (which > (uint32_t)free_count) {
        vga_write("partition: there is no free space with that number. Type partition.\n");
        return 1;
    }

    sectors = free_space[which - 1].sectors;
    if (size_mib > 0) {
        if (size_mib > sectors / 2048) {
            vga_write("partition: that free space only holds ");
            write_mib(sectors);
            vga_putchar('\n');
            return 1;
        }
        sectors = size_mib * 2048;
    }

    if (!confirmed) {
        vga_write("This creates a Fish partition from sector ");
        write_number(free_space[which - 1].start);
        vga_write(", ");
        write_mib(sectors);
        vga_write(",\nin space no partition uses. Nothing else on the disk changes.\n");
        vga_write("Type: partition new ");
        vga_write(words[1]);
        if (size_mib > 0) { vga_putchar(' '); vga_write(words[2]); }
        vga_write(" yes\n");
        return 1;
    }

    result = disk_create_fish(free_space[which - 1].start, sectors);
    if (result != DISK_OK) {
        vga_write("partition: ");
        vga_write(disk_error(result));
        vga_putchar('\n');
        return 1;
    }

    vga_write("Fish partition created. Type format yes to set up Fish on it.\n");
    return 0;
}

/* ---- PCI ---- */

static void write_hex_digits(uint32_t value, int digits)
{
    static const char hex[] = "0123456789abcdef";

    for (int i = digits - 1; i >= 0; i--) vga_putchar(hex[(value >> (i * 4)) & 0xF]);
}

static void write_pci_address(const struct pci_device *d)
{
    write_hex_digits(d->bus, 2);
    vga_putchar(':');
    write_hex_digits(d->slot, 2);
    vga_putchar('.');
    write_hex_digits(d->func, 1);
}

/* A BAR says where a device's own registers are. Bit 0 tells I/O ports
 * from memory; for memory, bits 1-2 say whether it is a 64-bit address
 * whose top half is in the next BAR. Returns how many BARs it used. */
static int write_bar(const struct pci_device *d, int i)
{
    uint32_t bar = d->bar[i];

    vga_write("  BAR");
    write_number((uint32_t)i);
    vga_write(": ");

    if (bar & 0x1) {
        vga_write("I/O ports at 0x");
        write_hex_digits(bar & ~0x3u, 4);
        vga_putchar('\n');
        return 1;
    }

    vga_write("memory at 0x");
    if (((bar >> 1) & 0x3) == 0x2 && i < 5) {
        if (d->bar[i + 1] != 0) write_hex_digits(d->bar[i + 1], 8);
        write_hex_digits(bar & ~0xFu, 8);
        vga_write(" (64-bit");
        vga_write((bar & 0x8) ? ", prefetchable)\n" : ")\n");
        return 2;
    }
    write_hex_digits(bar & ~0xFu, 8);
    vga_write((bar & 0x8) ? " (prefetchable)\n" : "\n");
    return 1;
}

/* pci        - one line per device
 * pci <n>    - everything about device n from that list */
static int cmd_pci(const char *arg)
{
    int count = pci_count();
    uint32_t which;
    const struct pci_device *d;

    if (arg[0] == '\0') {
        if (count == 0) { vga_write("No PCI devices found.\n"); return 1; }

        for (int i = 0; i < count; i++) {
            const char *vendor;

            d = pci_get(i);
            vendor = pci_vendor_name(d->vendor);

            if (i + 1 < 10) vga_putchar(' ');
            write_number((uint32_t)i + 1);
            vga_write("  ");
            write_pci_address(d);
            vga_write("  ");
            write_hex_digits(d->vendor, 4);
            vga_putchar(':');
            write_hex_digits(d->device, 4);
            vga_write("  ");
            vga_write(pci_class_name(d));
            if (vendor[0] != '\0') { vga_write(", "); vga_write(vendor); }
            vga_putchar('\n');
        }
        if (pci_found() > count) {
            vga_write("(and more: only the first ");
            write_number((uint32_t)count);
            vga_write(" are kept)\n");
        }
        return 0;
    }

    if (!parse_number(arg, &which) || which == 0 || which > (uint32_t)count) {
        vga_write("Usage: pci [number from the list]\n");
        return 1;
    }

    d = pci_get((int)which - 1);
    write_pci_address(d);
    vga_write("  ");
    vga_write(pci_class_name(d));
    vga_putchar('\n');

    vga_write("  Vendor 0x");
    write_hex_digits(d->vendor, 4);
    if (pci_vendor_name(d->vendor)[0] != '\0') {
        vga_write(" (");
        vga_write(pci_vendor_name(d->vendor));
        vga_putchar(')');
    }
    vga_write(", device 0x");
    write_hex_digits(d->device, 4);
    vga_write(", revision ");
    write_number(d->revision);
    vga_putchar('\n');

    vga_write("  Class ");
    write_hex_digits(d->class_code, 2);
    vga_putchar(':');
    write_hex_digits(d->subclass, 2);
    vga_putchar(':');
    write_hex_digits(d->prog_if, 2);
    vga_write(d->header_type == 1 ? ", PCI-to-PCI bridge\n" : "\n");

    for (int i = 0; i < 6; ) {
        if (d->bar[i] == 0) { i++; continue; }
        i += write_bar(d, i);
    }
    return 0;
}

static int cmd_color(const char *arg)
{
    int value = 0;

    if (arg[0] == '\0') {
        vga_write("Usage: color <0-15>\n");
        return 1;
    }

    for (size_t i = 0; arg[i] != '\0'; i++) {
        if (arg[i] < '0' || arg[i] > '9') {
            vga_write("color: not a number\n");
            return 1;
        }
        value = value * 10 + (arg[i] - '0');
    }

    if (value > 15) {
        vga_write("color: pick a number from 0 to 15\n");
        return 1;
    }

    vga_set_color((enum vga_color)value, VGA_BLACK);
    vga_write("Color changed.\n");

    return 0;
}

static int cmd_reboot(const char *arg)
{
    (void)arg;
    vga_write("Rebooting...\n");
    power_reboot();

    return 0;
}

static int cmd_halt(const char *arg)
{
    (void)arg;
    vga_write("System halted. You can close the system.\n");
    power_halt();

    return 0;
}

/* --- the command table ---
 *
 * Name, how to call it, one line of help, and the function. Keeping the
 * help text here means a new command is one row, not a row plus a line
 * somewhere else that is easy to forget. */

struct command {
    const char *name;
    const char *args;   /* how to call it, or "" when it takes nothing */
    const char *help;   /* one line, shown by the help command */
    int       (*run)(const char *arg);   /* 0 = success */
};

static const struct command commands[] = {
    { "help",     "[page]",         "show this list",                    cmd_help     },
    { "neofetch", "",               "show the logo",                     cmd_neofetch },
    { "clear",    "",               "clear the screen",                  cmd_clear    },
    { "echo",     "<text>",         "print text back",                   cmd_echo     },
    { "about",    "",               "information about this system",     cmd_about    },
    { "ls",       "",               "list files in the directory",       cmd_ls       },
    { "cd",       "<dir>",          "go in or out of a directory",       cmd_cd       },
    { "pwd",      "",               "print the current directory",       cmd_pwd      },
    { "mkdir",    "<name>",         "create a directory",                cmd_mkdir    },
    { "touch",    "<name>",         "create a file",                     cmd_touch    },
    { "cat",      "<file>",         "show what is inside a file",        cmd_cat      },
    { "write",    "<file> <text>",  "write text to a file",              cmd_write    },
    { "append",   "<file> <text>",  "add text to a file",                cmd_append   },
    { "rm",       "<name>",         "delete a file or empty directory",  cmd_rm       },
    { "yvim",     "<file>",         "edit a file (:wq saves, :q! quits)", cmd_yvim    },
    { "save",     "",               "write the filesystem to disk",      cmd_save     },
    { "load",     "",               "read the filesystem from disk",     cmd_load     },
    { "format",   "yes",            "erase the filesystem and save it",  cmd_format   },
    { "disk",     "[n [yes]]",      "list disks, or move Fish to disk n", cmd_disk    },
    { "partition", "[new <n>]",     "partitions, free space, new Fish",  cmd_partition },
    { "pci",      "[n]",            "list PCI devices, or show one",     cmd_pci      },
    { "color",    "<0-15>",         "change the text colour",            cmd_color    },
    { "whoami",   "",               "print the current user name",       cmd_whoami   },
    { "user",     "<name>",         "change the user name",              cmd_user     },
    { "hostname", "[name]",         "show or change the computer name",  cmd_hostname },
    { "uptime",   "",               "how long the system has run",       cmd_uptime   },
    { "ticks",    "",               "raw timer tick count",              cmd_ticks    },
    { "sleep",    "<secs>",         "wait for a while",                  cmd_sleep    },
    { "reboot",   "",               "restart the machine",               cmd_reboot   },
    { "halt",     "",               "stop the CPU",                      cmd_halt     },
};

#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

/* Twenty rows plus a header and a footer fits inside 25 lines. */
#define HELP_PER_PAGE 20
#define HELP_COLUMN   23

static size_t help_pages(void)
{
    return (COMMAND_COUNT + HELP_PER_PAGE - 1) / HELP_PER_PAGE;
}

/* Writes "  name args" and pads it out so the descriptions line up. */
static void write_padded(const char *name, const char *args)
{
    size_t width = 0;

    vga_write("  ");
    vga_write(name);
    width += strlen(name);

    if (args[0] != '\0') {
        vga_putchar(' ');
        width++;
        vga_write(args);
        width += strlen(args);
    }

    while (width < HELP_COLUMN) { vga_putchar(' '); width++; }
}

static int cmd_help(const char *arg)
{
    size_t page = 1;
    size_t pages = help_pages();
    size_t start, i;
    char buffer[12];

    if (arg[0] != '\0') {
        page = 0;
        for (size_t j = 0; arg[j] != '\0'; j++) {
            if (arg[j] < '0' || arg[j] > '9') {
                vga_write("help: not a number\n");
                return 1;
            }
            page = page * 10 + (size_t)(arg[j] - '0');
        }
        if (page < 1 || page > pages) {
            vga_write("help: no such page\n");
            return 1;
        }
    }

    start = (page - 1) * HELP_PER_PAGE;

    vga_write("Available commands (page ");
    utoa((unsigned int)page, buffer, 10);
    vga_write(buffer);
    vga_write(" of ");
    utoa((unsigned int)pages, buffer, 10);
    vga_write(buffer);
    vga_write("):\n");

    for (i = start; i < start + HELP_PER_PAGE && i < COMMAND_COUNT; i++) {
        write_padded(commands[i].name, commands[i].args);
        vga_write(commands[i].help);
        vga_putchar('\n');
    }

    if (page < pages) {
        vga_write("\nType help ");
        utoa((unsigned int)(page + 1), buffer, 10);
        vga_write(buffer);
        vga_write(" for more.\n");
    }

    return 0;
}

/* --- input --- */

/* --- command history ---
 *
 * The last HISTORY_SIZE commands, in a ring: 'history_next' is the slot
 * the next command goes into, so the newest one sits just before it.
 * Empty lines and a repeat of the previous command are not added. */

#define HISTORY_SIZE 16

static char   history[HISTORY_SIZE][LINE_MAX];
static size_t history_count;
static size_t history_next;

static void history_add(const char *text)
{
    const char *p = text;

    while (*p == ' ') p++;
    if (*p == '\0') return;

    if (history_count > 0) {
        size_t newest = (history_next + HISTORY_SIZE - 1) % HISTORY_SIZE;
        if (strcmp(history[newest], text) == 0) return;
    }

    strcpy(history[history_next], text);
    history_next = (history_next + 1) % HISTORY_SIZE;
    if (history_count < HISTORY_SIZE) history_count++;
}

/* Entry 0 is the newest command, 1 the one before it, and so on. */
static const char *history_get(size_t age)
{
    return history[(history_next + HISTORY_SIZE - 1 - age) % HISTORY_SIZE];
}

/* --- line editing ---
 *
 * The line lives in 'line', 'length' characters long, with the cursor at
 * 'pos'. The screen cursor always matches 'pos'. Anything that changes
 * the middle of the line reprints from there to the end and then walks
 * the screen cursor back - vga_cursor_left wraps across rows, so a long
 * line that spills onto the next row still works.
 *
 * All of this only ever touches what was typed on this line: the prompt
 * and anything printed before it are out of reach. */

static size_t length;
static size_t pos;

static void cursor_back(size_t count)
{
    while (count-- > 0) vga_cursor_left();
}

static void cursor_forward(size_t count)
{
    while (count-- > 0) vga_cursor_right();
}

/* Reprints from 'pos' to the end, blanks 'extra' cells after it (left
 * over from a line that just got shorter), and returns to 'pos'. */
static void redraw_tail(size_t extra)
{
    for (size_t i = pos; i < length; i++) vga_putchar(line[i]);
    for (size_t i = 0; i < extra; i++) vga_putchar(' ');
    cursor_back(length - pos + extra);
}

static void insert_key(char c)
{
    if (length >= LINE_MAX - 1) return;

    memmove(line + pos + 1, line + pos, length - pos);
    line[pos] = c;
    length++;

    vga_putchar(c);
    pos++;
    redraw_tail(0);
}

/* Deletes the character under the cursor. */
static void delete_at_cursor(void)
{
    if (pos >= length) return;

    memmove(line + pos, line + pos + 1, length - pos - 1);
    length--;
    redraw_tail(1);
}

/* Replaces the whole line, as when stepping through the history. */
static void replace_line(const char *text)
{
    size_t old = length;
    size_t i;

    cursor_back(pos);                     /* to the start of the line */
    pos = 0;

    for (i = 0; text[i] != '\0' && i < LINE_MAX - 1; i++) line[i] = text[i];
    length = i;

    redraw_tail(old > length ? old - length : 0);
    cursor_forward(length);
    pos = length;
}

/* Reads one line, with the cursor movable by Left, Right, Home and End,
 * Delete and Backspace working anywhere in it, and Up and Down walking
 * through the history. */
static void read_line(void)
{
    /* How far back in the history we are: 0 is the line being typed,
     * 1 the newest command, and so on. What was being typed before Up
     * was first pressed is kept in 'draft' so Down can bring it back. */
    size_t browsing = 0;
    static char draft[LINE_MAX];

    length = 0;
    pos = 0;

    for (;;) {
        int key = keyboard_getchar();

        if (key == 0) {
            power_idle();
            continue;
        }

        switch (key) {
        case '\n':
            cursor_forward(length - pos);
            vga_putchar('\n');
            line[length] = '\0';
            history_add(line);
            return;

        case '\b':
            if (pos > 0) {                /* this is the boundary */
                vga_cursor_left();
                pos--;
                delete_at_cursor();
            }
            break;

        case KEY_DELETE:
            delete_at_cursor();
            break;

        case KEY_LEFT:
            if (pos > 0) { vga_cursor_left(); pos--; }
            break;

        case KEY_RIGHT:
            if (pos < length) { vga_cursor_right(); pos++; }
            break;

        case KEY_HOME:
            cursor_back(pos);
            pos = 0;
            break;

        case KEY_END:
            cursor_forward(length - pos);
            pos = length;
            break;

        case KEY_UP:
            if (browsing < history_count) {
                if (browsing == 0) {
                    line[length] = '\0';
                    strcpy(draft, line);
                }
                browsing++;
                replace_line(history_get(browsing - 1));
            }
            break;

        case KEY_DOWN:
            if (browsing > 0) {
                browsing--;
                replace_line(browsing == 0 ? draft : history_get(browsing - 1));
            }
            break;

        default:
            /* Ignore anything else without a character. */
            if (key >= ' ' && key <= 255) insert_key((char)key);
            break;
        }
    }
}

/* Splits the line into a command and the rest. Returns a pointer to the
 * argument, and cuts the command short with a '\0'. */
static char *split(char *text)
{
    size_t i = 0;

    while (text[i] != '\0' && text[i] != ' ') i++;

    if (text[i] == '\0') return text + i;   /* no argument: empty string */

    text[i] = '\0';
    i++;
    while (text[i] == ' ') i++;             /* skip extra spaces */
    return text + i;
}

/* Exit status for a command that does not exist, as in other shells. */
#define STATUS_NOT_FOUND 127

static int is_blank(const char *text)
{
    while (*text == ' ') text++;
    return *text == '\0';
}

/* Runs one command, with no operators in it. Returns its status: 0 for
 * success, anything else for failure. */
static int run_command(char *text)
{
    char *arg;
    size_t end;

    /* Trim spaces on both sides: "ls && pwd" hands us "ls " and " pwd". */
    while (*text == ' ') text++;
    end = strlen(text);
    while (end > 0 && text[end - 1] == ' ') text[--end] = '\0';
    if (*text == '\0') return 0;

    arg = split(text);

    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (strcmp(text, commands[i].name) == 0) return commands[i].run(arg);
    }

    vga_write(text);
    vga_write(": command not found. Type help.\n");
    return STATUS_NOT_FOUND;
}

/* --- chaining commands ---
 *
 *   a && b    run b only if a succeeded
 *   a || b    run b only if a failed
 *   a |& b    run b whatever happened to a
 *   a ;  b    the same, the way most shells spell it
 *
 * They read left to right, and a command that is skipped leaves the
 * status as it was, so "a && b || c" runs c when either a or b fails. */

#define MAX_CHAIN 16

enum chain { CHAIN_ALWAYS, CHAIN_AND, CHAIN_OR };

static void execute(char *text)
{
    char       *parts[MAX_CHAIN];
    enum chain  joins[MAX_CHAIN];       /* how parts[i] hangs on parts[i-1] */
    const char *names[MAX_CHAIN];       /* the operator as typed, for errors */
    size_t      count = 1;
    int         status = 0;
    char       *p = text;

    parts[0] = text;
    joins[0] = CHAIN_ALWAYS;
    names[0] = "";

    /* Cut the line at each operator. */
    while (*p != '\0') {
        enum chain  join;
        const char *name;
        size_t      width;

        if      (p[0] == '&' && p[1] == '&') { join = CHAIN_AND;    name = "&&"; width = 2; }
        else if (p[0] == '|' && p[1] == '|') { join = CHAIN_OR;     name = "||"; width = 2; }
        else if (p[0] == '|' && p[1] == '&') { join = CHAIN_ALWAYS; name = "|&"; width = 2; }
        else if (p[0] == ';')                { join = CHAIN_ALWAYS; name = ";";  width = 1; }
        else { p++; continue; }

        if (count == MAX_CHAIN) {
            vga_write("Too many commands on one line.\n");
            return;
        }

        *p = '\0';
        p += width;
        parts[count] = p;
        joins[count] = join;
        names[count] = name;
        count++;
    }

    /* An operator needs a command on each side - "ls &&" or "&& ls" is
     * a mistake, and running half of it would be worse than refusing.
     * The exception is a trailing ";", which other shells allow too. */
    if (count > 1) {
        for (size_t i = 0; i < count; i++) {
            if (!is_blank(parts[i])) continue;
            if (i == count - 1 && strcmp(names[i], ";") == 0) continue;

            vga_write("Syntax error near ");
            vga_write(names[i == 0 ? 1 : i]);
            vga_putchar('\n');
            return;
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (joins[i] == CHAIN_AND && status != 0) continue;
        if (joins[i] == CHAIN_OR  && status == 0) continue;
        status = run_command(parts[i]);
    }
}

void shell_run(void)
{
    char path[FISH_PATH_MAX];

    for (;;) {
        /* user@host /current/dir >> */
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_write(user_name());
        vga_putchar('@');
        vga_write(user_host());
        vga_putchar(' ');

        fish_path(path, sizeof(path));
        vga_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
        vga_write(path);

        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_write(" >> ");

        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        read_line();

        execute(line);
    }
}