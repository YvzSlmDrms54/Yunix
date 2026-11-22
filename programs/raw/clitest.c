/* programs/raw/clitest.c - tries an instruction only the kernel may use.
 *
 * "cli" turns interrupts off. In ring 3 the CPU refuses it with a
 * general protection fault, and Yunix must stop the program - otherwise
 * any program could freeze the machine.
 */

static void say(const char *s, int n)
{
    __asm__ volatile ("int $0x80" : : "a"(4), "b"(1), "c"(s), "d"(n) : "memory");
}

void _start(void)
{
    say("clitest: trying cli from ring 3\n", 32);
    __asm__ volatile ("cli");
    say("clitest: WRONG - cli went through\n", 34);
    __asm__ volatile ("int $0x80" : : "a"(1), "b"(1));
    for (;;) { }
}
