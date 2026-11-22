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

#define SYS_EXIT        1
#define SYS_WRITE       4
#define SYS_EXIT_GROUP  252

#define EBADF           9
#define EFAULT          14
#define ENOSYS          38

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
