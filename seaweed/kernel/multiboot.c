/* seaweed/kernel/multiboot.c - what the boot loader handed over.
 *
 * A multiboot loader such as GRUB leaves a pointer to an information
 * block in EBX. Its first word is a set of flags saying which of the
 * fields after it are filled in. Seaweed uses one of them so far:
 *
 *     flags bit 3   mods_count (offset 20) and mods_addr (offset 24)
 *
 * mods_addr points to an array of 16-byte entries: where a module starts,
 * where it ends, and the address of its name - the text after the path
 * on GRUB's "module" line.
 *
 * Everything here came from outside the kernel, so it is checked before
 * use: a module must lie inside the identity-mapped memory, and its name
 * must end within a reasonable length.
 */

#include "multiboot.h"
#include "paging.h"
#include "string.h"

#define FLAG_MODS     (1u << 3)
#define MAX_MODULES   8
#define NAME_MAX      32

/* The end of the memory paging maps, and so of what we can trust. */
#define MAPPED_END    ((uint32_t)PAGING_MAPPED_MB * 1024 * 1024)

struct module {
    uint32_t start;
    uint32_t end;
    char     name[NAME_MAX];
};

static struct module modules[MAX_MODULES];
static int           module_count;

/* Is [address, address + size) inside mapped memory and not in page 0? */
static int readable(uint32_t address, uint32_t size)
{
    return address >= 0x1000 && address < MAPPED_END && size <= MAPPED_END - address;
}

/* Copies a module's name out of boot loader memory. Loaders disagree on
 * what the "string" holds: GRUB passes the words after the path, QEMU's
 * -initrd passes the path and then those words. Taking the last word,
 * without any directories in front of it, gives the same name for both -
 * and a module given only a path is named after its file.
 * Returns 0 if the text never ends or the name does not fit. */
static int copy_name(uint32_t address, char *out)
{
    const char *text = (const char *)address;
    size_t length = 0, start, end;

    while (1) {
        if (length == 256 || !readable(address + length, 1)) return 0;
        if (text[length] == '\0') break;
        length++;
    }

    end = length;
    while (end > 0 && text[end - 1] == ' ') end--;
    start = end;
    while (start > 0 && text[start - 1] != ' ' && text[start - 1] != '/') start--;

    if (end == start || end - start >= NAME_MAX) return 0;
    memcpy(out, text + start, end - start);
    out[end - start] = '\0';
    return 1;
}

void multiboot_init(uint32_t magic, uint32_t info)
{
    const uint32_t *mbi = (const uint32_t *)info;
    uint32_t count, list;

    module_count = 0;

    if (magic != MULTIBOOT_MAGIC || !readable(info, 28)) return;
    if (!(mbi[0] & FLAG_MODS)) return;

    count = mbi[5];
    list = mbi[6];
    if (count > MAX_MODULES) count = MAX_MODULES;
    if (count == 0 || !readable(list, count * 16)) return;

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t *entry = (const uint32_t *)(list + i * 16);
        struct module *m = &modules[module_count];

        if (entry[1] <= entry[0] || !readable(entry[0], entry[1] - entry[0])) continue;
        if (!copy_name(entry[2], m->name)) continue;

        m->start = entry[0];
        m->end = entry[1];
        module_count++;
    }
}

const void *multiboot_module(const char *name, size_t *size_out)
{
    for (int i = 0; i < module_count; i++) {
        if (strcmp(modules[i].name, name) == 0) {
            if (size_out != 0) *size_out = modules[i].end - modules[i].start;
            return (const void *)modules[i].start;
        }
    }
    return 0;
}

int multiboot_module_count(void)
{
    return module_count;
}

const char *multiboot_module_name(int index)
{
    if (index < 0 || index >= module_count) return 0;
    return modules[index].name;
}
