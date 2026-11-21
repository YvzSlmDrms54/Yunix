/* seaweed/kernel/power.c - idling, rebooting and halting the CPU.
 *
 * These used to live in the shell, poking I/O ports directly. Port I/O is
 * the kernel's job, so they moved here and Yunix asks for them through
 * seaweed.h.
 */

#include "seaweed.h"
#include "io.h"

void power_idle(void)
{
    __asm__ volatile ("hlt");
}

/* Asks the old keyboard controller to pulse the CPU reset line. It is a
 * strange place for a reset button, but on a PC that is where it lives. */
void power_reboot(void)
{
    /* Wait for the controller's input buffer to empty - but not forever.
     * A missing or stuck controller must not hang the machine here. */
    for (uint32_t i = 0; i < 1000000 && (inb(0x64) & 0x02); i++) { }

    outb(0x64, 0xFE);

    /* If the reset did not happen, at least stop cleanly. */
    for (;;) __asm__ volatile ("cli; hlt");
}

void power_halt(void)
{
    for (;;) __asm__ volatile ("cli; hlt");
}
