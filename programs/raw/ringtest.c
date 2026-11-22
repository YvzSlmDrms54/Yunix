/* programs/raw/ringtest.c - checks that ring 3 is really a cage.
 *
 * It asks the kernel for things it should refuse, spins long enough for
 * timer interrupts to arrive while it runs in ring 3, and at the end
 * reaches into kernel memory - which must get it stopped, not crash
 * Yunix.
 */

static int syscall3(int number, int a, int b, int c)
{
    int result;
    __asm__ volatile ("int $0x80"
                      : "=a"(result)
                      : "a"(number), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return result;
}

static int length(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void say(const char *s) { syscall3(4, 1, (int)s, length(s)); }

static void say_number(int value)
{
    char buffer[12];
    int i = 11, negative = value < 0;
    unsigned int v = negative ? 0u - (unsigned int)value : (unsigned int)value;

    buffer[i] = '\0';
    do { buffer[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    if (negative) buffer[--i] = '-';
    say(buffer + i);
}

static void check(const char *what, int got, int want)
{
    say(got == want ? "  ok    " : "  WRONG ");
    say(what);
    say(" -> ");
    say_number(got);
    say("\n");
}

void _start(void)
{
    volatile unsigned int spin;

    say("ringtest: asking the kernel for things it must refuse\n");
    check("write to fd 7 (no such file), want -9 EBADF", syscall3(4, 7, (int)"x", 1), -9);
    check("write from kernel memory 0x100000, want -14 EFAULT", syscall3(4, 1, 0x100000, 4), -14);
    check("write running past the stack top, want -14 EFAULT", syscall3(4, 1, (int)0xBFFFFFF0, 64), -14);
    check("system call 999, want -38 ENOSYS", syscall3(999, 0, 0, 0), -38);

    say("ringtest: spinning, so timer interrupts arrive in ring 3...\n");
    for (spin = 0; spin < 200000000u; spin++) { }
    say("ringtest: still alive after the interrupts\n");

    say("ringtest: now reading kernel memory at 0x100000 - this must be stopped\n");
    spin = *(volatile unsigned int *)0x100000;

    say("ringtest: WRONG - the read went through\n");
    syscall3(1, 1, 0, 0);
    for (;;) { }
}
