#ifndef PROGRAM_H
#define PROGRAM_H

/* Running ring 3 programs. The listing and program_run are public, in
 * seaweed.h; these are for the rest of the kernel. */

#include <stdint.h>

/* 1 while a program runs. Exceptions use it to tell a program's crash
 * from the kernel's. */
int program_running(void);

/* Ends the running program with this status, as a Linux shell would
 * see it: 0-255 from exit(), 128 + signal for a crash. Does not return:
 * program_run returns instead. */
void program_end(uint32_t status) __attribute__((noreturn));

/* The name of the running program, for messages. */
const char *program_current_name(void);

/* Linux exit statuses for a program killed by a signal. */
#define STATUS_SIGILL  (128 + 4)
#define STATUS_SIGFPE  (128 + 8)
#define STATUS_SIGSEGV (128 + 11)

#endif
