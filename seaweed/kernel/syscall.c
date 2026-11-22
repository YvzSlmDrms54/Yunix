/* seaweed/kernel/syscall.c - what a program asks the kernel for.
 *
 * Seaweed speaks the 32-bit Linux system call convention, so programs
 * built for Linux - and C libraries written for it, like musl - work
 * unchanged:
 *
 *     int $0x80     the call itself
 *     EAX           which call, by Linux's number
 *     EBX ECX EDX ESI EDI EBP   up to six arguments
 *     EAX           the result; -1 to -4095 mean an error (minus errno)
 *
 * The numbers and error codes below are Linux's own (from
 * arch/x86/entry/syscalls/syscall_32.tbl and errno.h). Seaweed is not
 * Linux and shares no code with it - only this calling convention.
 *
 * Every pointer a program passes is checked before use: a program must
 * not be able to make the kernel read or write anything it could not
 * reach itself.
 */

#include "seaweed.h"
#include "idt.h"
#include "paging.h"
#include "program.h"
#include "string.h"
#include "gdt.h"

#define SYS_EXIT            1
#define SYS_WRITE           4
#define SYS_IOCTL           54
#define SYS_WRITEV          146
#define SYS_SET_THREAD_AREA 243
#define SYS_EXIT_GROUP      252
#define SYS_SET_TID_ADDRESS 258

#define EBADF           9
#define EFAULT          14
#define EINVAL          22
#define ENOTTY          25
#define ENOSYS          38

#define TIOCGWINSZ      0x5413

/* With one program at a time, it is process 1 and its only thread. */
#define ONLY_TID        1

#define STDOUT          1
#define STDERR          2

static int32_t sys_write(uint32_t fd, uint32_t buffer, uint32_t count)
{
    if (fd != STDOUT && fd != STDERR) return -EBADF;      /* no files yet */
    if (!paging_user_range_ok(buffer, count)) return -EFAULT;

    for (uint32_t i = 0; i < count; i++) {
        vga_putchar(*(const char *)paging_user_physical(buffer + i));
    }
    return (int32_t)count;
}

/* Copies between kernel and program memory, a byte at a time through the
 * program's page tables. The caller has checked the range. */
static void copy_from_user(void *to, uint32_t from, uint32_t size)
{
    uint8_t *out = to;
    for (uint32_t i = 0; i < size; i++) out[i] = *(const uint8_t *)paging_user_physical(from + i);
}

static void copy_to_user(uint32_t to, const void *from, uint32_t size)
{
    const uint8_t *in = from;
    for (uint32_t i = 0; i < size; i++) *(uint8_t *)paging_user_physical(to + i) = in[i];
}

/* writev: several buffers in one call - what printf really uses. Each
 * struct iovec is a base and a length. */
static int32_t sys_writev(uint32_t fd, uint32_t iov, uint32_t count)
{
    uint32_t total = 0;

    if (fd != STDOUT && fd != STDERR) return -EBADF;
    if (count > 1024) return -EINVAL;
    if (!paging_user_range_ok(iov, count * 8)) return -EFAULT;

    /* Check every buffer before writing any, so a bad one in the middle
     * does not leave half the output on the screen. */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry[2];
        copy_from_user(entry, iov + i * 8, 8);
        if (!paging_user_range_ok(entry[0], entry[1])) return -EFAULT;
        if (total + entry[1] < total) return -EINVAL;
        total += entry[1];
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry[2];
        copy_from_user(entry, iov + i * 8, 8);
        sys_write(fd, entry[0], entry[1]);
    }
    return (int32_t)total;
}

/* ioctl: only the one question C libraries ask about the screen - how big
 * is the terminal? musl uses the answer to decide that stdout is a
 * terminal, and so flushes it after every line. */
static int32_t sys_ioctl(uint32_t fd, uint32_t request, uint32_t argument)
{
    if (fd > STDERR) return -EBADF;

    if (request == TIOCGWINSZ) {
        uint16_t size[4] = { VGA_HEIGHT, VGA_WIDTH, 0, 0 };   /* rows, columns, pixels */

        if (!paging_user_range_ok(argument, sizeof(size))) return -EFAULT;
        copy_to_user(argument, size, sizeof(size));
        return 0;
    }
    return -ENOTTY;
}

/* set_thread_area: give the program a segment for its thread-local
 * storage. It passes a struct user_desc; entry_number -1 means "pick one
 * for me", and the kernel writes back which it picked. The program then
 * loads (entry << 3) | 3 into gs. */
static int32_t sys_set_thread_area(uint32_t descriptor)
{
    uint32_t desc[4];              /* entry_number, base_addr, limit, flags */
    int32_t entry;
    uint32_t flags;

    if (!paging_user_range_ok(descriptor, sizeof(desc))) return -EFAULT;
    copy_from_user(desc, descriptor, sizeof(desc));

    entry = (int32_t)desc[0];
    if (entry == -1) {
        entry = GDT_TLS_FIRST;
        desc[0] = (uint32_t)entry;
        copy_to_user(descriptor, desc, 4);
    }
    if (entry < GDT_TLS_FIRST || entry >= GDT_TLS_FIRST + GDT_TLS_COUNT) return -EINVAL;

    /* flags bits: 0 seg_32bit, 1-2 contents, 3 read_exec_only,
     * 4 limit_in_pages, 5 seg_not_present, 6 useable */
    flags = desc[3];
    gdt_set_tls(entry, desc[1], desc[2] & 0xFFFFF, (flags >> 4) & 1, !((flags >> 5) & 1));
    return 0;
}

/* Called from syscall_stub (boot/syscall.s) with the program's registers. */
void syscall_handler(struct registers *regs)
{
    int32_t result;

    if (!program_running()) { regs->eax = (uint32_t)-ENOSYS; return; }

    switch (regs->eax) {
    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        program_end(regs->ebx & 0xFF);              /* does not return */

    case SYS_WRITE:
        result = sys_write(regs->ebx, regs->ecx, regs->edx);
        break;

    case SYS_WRITEV:
        result = sys_writev(regs->ebx, regs->ecx, regs->edx);
        break;

    case SYS_IOCTL:
        result = sys_ioctl(regs->ebx, regs->ecx, regs->edx);
        break;

    case SYS_SET_THREAD_AREA:
        result = sys_set_thread_area(regs->ebx);
        break;

    case SYS_SET_TID_ADDRESS:
        /* Where to write 0 when the thread ends; with one thread, nothing
         * waits on it. The answer is the thread's id. */
        result = ONLY_TID;
        break;

    default:
        /* Say which call was missing: when a Linux program does not run,
         * this is the list of what to build next. */
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_write("[seaweed] system call ");
        {
            char buffer[12];
            utoa(regs->eax, buffer, 10);
            vga_write(buffer);
        }
        vga_write(" is not supported yet\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        result = -ENOSYS;
        break;
    }

    regs->eax = (uint32_t)result;
}
