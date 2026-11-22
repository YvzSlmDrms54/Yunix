/* programs/raw/tiny.c - the first ring 3 program, with no C library.
 *
 * No C library yet, so it makes its two system calls by hand, the way
 * any 32-bit Linux program does underneath: number in EAX, arguments in
 * EBX, ECX, EDX, then "int $0x80". Because of that, the very same file
 * runs on Linux too.
 */

static int sys_write(int fd, const char *text, int length)
{
    int result;
    __asm__ volatile ("int $0x80"
                      : "=a"(result)
                      : "a"(4), "b"(fd), "c"(text), "d"(length)
                      : "memory");
    return result;
}

static void sys_exit(int status)
{
    __asm__ volatile ("int $0x80" : : "a"(1), "b"(status));
    for (;;) { }
}

void _start(void)
{
    static const char message[] = "Hello from ring 3!\n";

    sys_write(1, message, sizeof(message) - 1);
    sys_exit(0);
}
