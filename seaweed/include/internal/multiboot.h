#ifndef MULTIBOOT_H
#define MULTIBOOT_H

/* What the boot loader told us. Only the parts Seaweed uses. */

#include <stdint.h>
#include <stddef.h>

#define MULTIBOOT_MAGIC 0x2BADB002u

/* Reads the boot loader's information block and remembers the modules
 * it loaded. Call first thing in kernel_main, with its two arguments. */
void multiboot_init(uint32_t magic, uint32_t info);

/* A module the boot loader loaded, found by the name given after its
 * path in grub.cfg (or in QEMU's -initrd). Returns its start, and its
 * size in 'size_out', or NULL if there is no such module. */
const void *multiboot_module(const char *name, size_t *size_out);

/* For listing them: how many there are, and the name of each. */
int multiboot_module_count(void);
const char *multiboot_module_name(int index);

#endif
