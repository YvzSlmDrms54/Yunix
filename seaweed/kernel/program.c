/* seaweed/kernel/program.c - loading and running ring 3 programs.
 *
 * A program is a static 32-bit ELF file - the same kind Linux runs. For
 * now they come in as boot modules (from the ISO, or QEMU's -initrd);
 * reading them from Fish comes later.
 *
 * Loading one:
 *   1. check the ELF header: 32-bit, little-endian, x86, an executable
 *   2. for each PT_LOAD segment, give its pages memory from the program
 *      pool, copy in the bytes from the file and zero the rest (that rest
 *      is .bss, the variables that start at zero)
 *   3. build the stack the way Linux does - argc, argv, envp and the
 *      "auxiliary vector" with facts about the process
 *   4. drop to ring 3 at the entry point, and wait for exit or a crash
 *
 * Only one program runs at a time, in one shared set of user pages that
 * is wiped when it ends. Several at once needs processes - later.
 */

#include "seaweed.h"
#include "program.h"
#include "paging.h"
#include "multiboot.h"
#include "string.h"
#include "gdt.h"

#define PAGE            4096u
#define POOL_PAGES      256u               /* 1 MiB for code, data and heap */
#define STACK_PAGES     16u                /* 64 KiB of stack */

/* ELF, as much as we read of it. */
#define ELF_CLASS_32    1
#define ELF_DATA_LE     1
#define ELF_TYPE_EXEC   2
#define ELF_MACHINE_386 3
#define PT_LOAD         1

/* Auxiliary vector entries (from Linux's elf.h). */
#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_ENTRY        9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_SECURE       23
#define AT_RANDOM       25

struct elf_header {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf_program_header {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed));

/* Physical memory for programs. Plain kernel memory, identity-mapped,
 * handed out a page at a time and mapped at user addresses. */
static uint8_t  pool[(POOL_PAGES + STACK_PAGES) * PAGE] __attribute__((aligned(PAGE)));
static uint32_t pool_used;

static int         running;
static const char *running_name;

/* Defined in boot/syscall.s */
extern uint32_t user_enter(uint32_t entry, uint32_t user_stack);
extern void     user_leave(uint32_t status) __attribute__((noreturn));

/* --- names --- */

/* Boot modules that are not programs. */
static int is_system_module(const char *name)
{
    return strcmp(name, "seaweed.bin") == 0 || strcmp(name, "boot.img") == 0 ||
           strcmp(name, "core.img") == 0;
}

static int is_program(const char *name)
{
    size_t size;
    const uint8_t *data = multiboot_module(name, &size);

    return data != 0 && !is_system_module(name) && size >= 4 &&
           memcmp(data, "\x7F" "ELF", 4) == 0;
}

int program_count(void)
{
    int n = 0;
    for (int i = 0; i < multiboot_module_count(); i++) {
        if (is_program(multiboot_module_name(i))) n++;
    }
    return n;
}

const char *program_name(int index)
{
    for (int i = 0; i < multiboot_module_count(); i++) {
        const char *name = multiboot_module_name(i);
        if (is_program(name) && index-- == 0) return name;
    }
    return 0;
}

/* --- memory --- */

static uint32_t take_page(void)
{
    uint32_t page;

    if (pool_used == POOL_PAGES + STACK_PAGES) return 0;
    page = (uint32_t)pool + pool_used * PAGE;
    pool_used++;
    memset((void *)page, 0, PAGE);
    return page;
}

/* Gives every page of [start, end) memory, unless it already has some. */
static int map_range(uint32_t start, uint32_t end)
{
    for (uint32_t page = start & ~(PAGE - 1); page < end; page += PAGE) {
        uint32_t physical;

        if (paging_user_physical(page) != 0) continue;
        physical = take_page();
        if (physical == 0 || paging_map_user(page, physical) != 0) return -1;
    }
    return 0;
}

/* Copies into program memory through the physical pages - the kernel
 * never needs to trust the program's own mappings for this. */
static void copy_in(uint32_t address, const void *source, uint32_t size)
{
    const uint8_t *in = source;

    for (uint32_t i = 0; i < size; i++) {
        *(uint8_t *)paging_user_physical(address + i) = in[i];
    }
}

static void put_word(uint32_t address, uint32_t value)
{
    copy_in(address, &value, 4);
}

/* --- loading --- */

static int load(const uint8_t *file, size_t size, uint32_t *entry, uint32_t *phdr_address)
{
    const struct elf_header *h = (const struct elf_header *)file;

    if (size < sizeof(*h) || memcmp(h->ident, "\x7F" "ELF", 4) != 0) return PROGRAM_ERR_NOT_ELF;
    if (h->ident[4] != ELF_CLASS_32 || h->ident[5] != ELF_DATA_LE ||
        h->machine != ELF_MACHINE_386) return PROGRAM_ERR_NOT_ELF;
    if (h->type != ELF_TYPE_EXEC) return PROGRAM_ERR_UNSUPPORTED;     /* PIE or a library */
    if (h->phentsize != sizeof(struct elf_program_header) || h->phnum == 0 || h->phnum > 16 ||
        h->phoff > size || (size_t)h->phnum * h->phentsize > size - h->phoff) {
        return PROGRAM_ERR_NOT_ELF;
    }

    *phdr_address = 0;

    for (int i = 0; i < h->phnum; i++) {
        const struct elf_program_header *p =
            (const struct elf_program_header *)(file + h->phoff + i * sizeof(*p));

        if (p->type != PT_LOAD || p->memsz == 0) continue;

        /* Everything must lie inside the file, and inside the window
         * programs live in - checked without letting sums wrap around. */
        if (p->filesz > p->memsz || p->offset > size || p->filesz > size - p->offset) {
            return PROGRAM_ERR_NOT_ELF;
        }
        if (p->vaddr < PAGING_USER_CODE_START || p->vaddr >= PAGING_USER_CODE_END ||
            p->memsz > PAGING_USER_CODE_END - p->vaddr) {
            return PROGRAM_ERR_UNSUPPORTED;
        }

        if (map_range(p->vaddr, p->vaddr + p->memsz) != 0) return PROGRAM_ERR_TOO_BIG;
        copy_in(p->vaddr, file + p->offset, p->filesz);   /* the rest is already zero */

        /* Where the program headers ended up in memory: the C library
         * finds things like thread-local storage through them. */
        if (h->phoff >= p->offset && h->phoff - p->offset < p->filesz) {
            *phdr_address = p->vaddr + (h->phoff - p->offset);
        }
    }

    if (h->entry < PAGING_USER_CODE_START || h->entry >= PAGING_USER_CODE_END ||
        paging_user_physical(h->entry) == 0) {
        return PROGRAM_ERR_NOT_ELF;
    }
    *entry = h->entry;
    return PROGRAM_OK;
}

/* The stack a Linux program starts with, from the bottom (where ESP
 * points) up:
 *
 *     argc
 *     argv[0] ... argv[argc - 1], NULL
 *     envp[0] ... , NULL                    (none yet)
 *     auxiliary vector: (type, value) pairs, ending with AT_NULL
 *     ... and above all that, the strings and bytes they point to
 */
static uint32_t build_stack(const char *name, const uint8_t *file, uint32_t entry,
                            uint32_t phdr_address)
{
    const struct elf_header *h = (const struct elf_header *)file;
    uint32_t top = PAGING_USER_STACK_END;
    uint32_t name_address, random_address, sp;
    uint32_t low, high;
    uint32_t words[32];
    int n = 0;

    if (map_range(top - STACK_PAGES * PAGE, top) != 0) return 0;

    /* 16 random bytes: the C library seeds its stack protector with them. */
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    top -= 16;
    random_address = top;
    for (int i = 0; i < 4; i++) put_word(top + i * 4, (low ^ (high << i) ^ timer_ticks()) * 2654435761u + i);

    top -= (uint32_t)strlen(name) + 1;
    name_address = top;
    copy_in(name_address, name, (uint32_t)strlen(name) + 1);

    words[n++] = 1;                                     /* argc */
    words[n++] = name_address;                          /* argv[0] */
    words[n++] = 0;                                     /* end of argv */
    words[n++] = 0;                                     /* end of envp */
    if (phdr_address) {
        words[n++] = AT_PHDR;  words[n++] = phdr_address;
        words[n++] = AT_PHENT; words[n++] = h->phentsize;
        words[n++] = AT_PHNUM; words[n++] = h->phnum;
    }
    words[n++] = AT_PAGESZ; words[n++] = PAGE;
    words[n++] = AT_ENTRY;  words[n++] = entry;
    /* Who the program runs as: root, for now. Saying so matters - musl
     * treats a program that does not learn its user ids as possibly
     * privileged, and then probes its file descriptors with poll(). */
    words[n++] = AT_UID;    words[n++] = 0;
    words[n++] = AT_EUID;   words[n++] = 0;
    words[n++] = AT_GID;    words[n++] = 0;
    words[n++] = AT_EGID;   words[n++] = 0;
    words[n++] = AT_SECURE; words[n++] = 0;
    words[n++] = AT_RANDOM; words[n++] = random_address;
    words[n++] = AT_NULL;   words[n++] = 0;

    /* Linux hands over ESP 16-byte aligned. */
    sp = (top - (uint32_t)n * 4) & ~15u;
    for (int i = 0; i < n; i++) put_word(sp + (uint32_t)i * 4, words[i]);
    return sp;
}

/* --- running --- */

int program_run(const char *name)
{
    const uint8_t *file;
    size_t size;
    uint32_t entry, phdr_address, sp, status;
    int result;

    if (running) return PROGRAM_ERR_BUSY;
    if (name == 0 || !is_program(name)) return PROGRAM_ERR_NOT_FOUND;
    file = multiboot_module(name, &size);

    pool_used = 0;
    result = load(file, size, &entry, &phdr_address);
    if (result == PROGRAM_OK) {
        sp = build_stack(name, file, entry, phdr_address);
        if (sp == 0) result = PROGRAM_ERR_TOO_BIG;
    }
    if (result != PROGRAM_OK) {
        paging_unmap_user();
        return result;
    }

    running = 1;
    running_name = name;
    __asm__ volatile ("fninit");            /* a clean FPU for each program */
    status = user_enter(entry, sp);
    running = 0;
    running_name = 0;

    paging_unmap_user();
    gdt_clear_tls();
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    return (int)(status & 0xFF);
}

int program_running(void)
{
    return running;
}

const char *program_current_name(void)
{
    return running_name ? running_name : "program";
}

void program_end(uint32_t status)
{
    user_leave(status);
}

const char *program_error(int code)
{
    switch (code) {
    case PROGRAM_ERR_NOT_FOUND:   return "no such program";
    case PROGRAM_ERR_NOT_ELF:     return "not a valid 32-bit x86 ELF program";
    case PROGRAM_ERR_UNSUPPORTED: return "Yunix cannot load this kind of program yet (static, non-PIE only)";
    case PROGRAM_ERR_TOO_BIG:     return "the program does not fit in the 1 MiB programs get";
    case PROGRAM_ERR_BUSY:        return "a program is already running";
    default:                      return "unknown error";
    }
}
