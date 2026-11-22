/* programs/libctest.c - more of the C library at work.
 *
 * Formatting (including floating point, which runs on the x87 FPU),
 * sorting, string conversion, and errno - which musl keeps in
 * thread-local storage, reached through the gs segment. Reading errno
 * back after a long spin checks that gs survives thousands of timer
 * interrupts.
 *
 * Ends with status 3 on purpose, so the shell has something to show.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ascending(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

int main(void)
{
    int numbers[] = { 42, 7, 19, 3, 88, 23 };
    size_t count = sizeof(numbers) / sizeof(numbers[0]);
    char text[64];
    volatile unsigned long spin;
    double third = 1.0 / 3.0;
    void *memory;

    printf("libctest: printf %d %u %x %s %c\n", -5, 4000000000u, 0xBEEF, "text", 'Y');
    printf("libctest: floating point %.4f %e %g\n", third, 12345.678, 0.5);

    qsort(numbers, count, sizeof(numbers[0]), ascending);
    printf("libctest: sorted");
    for (size_t i = 0; i < count; i++) printf(" %d", numbers[i]);
    printf("\n");

    snprintf(text, sizeof(text), "%s-%04d", "yunix", 7);
    printf("libctest: snprintf \"%s\", strlen %zu, strtol(\"0x1F\") %ld\n",
           text, strlen(text), strtol("0x1F", NULL, 16));

    errno = 0;
    if (write(42, "x", 1) == -1) {
        printf("libctest: write to fd 42 failed, errno %d (%s)\n", errno, strerror(errno));
    }
    for (spin = 0; spin < 150000000ul; spin++) { }
    printf("libctest: errno after a long spin is still %d %s\n",
           errno, errno == EBADF ? "(ok)" : "(WRONG)");

    memory = malloc(100);
    printf("libctest: malloc(100) gave %p\n", memory);

    fprintf(stderr, "libctest: this line went to stderr\n");
    return 3;
}
