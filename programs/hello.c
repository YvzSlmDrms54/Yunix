/* programs/hello.c - an ordinary C program.
 *
 * Nothing in here knows about Yunix: it is plain C with stdio, built
 * against musl, and the same file runs on Linux. That it runs on Yunix
 * too is the whole point - Seaweed answers the Linux system calls musl
 * makes underneath.
 */

#include <stdio.h>

int main(int argc, char **argv)
{
    printf("Hello from musl on Yunix!\n");
    printf("I am %s, and I got %d argument%s.\n", argv[0], argc, argc == 1 ? "" : "s");
    return 0;
}
