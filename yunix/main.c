/* yunix/main.c - where Yunix starts.
 *
 * Seaweed calls yunix_main() once the hardware is up and Fish has been
 * loaded. From here on everything is Yunix: the user settings, the
 * shell, and later the programs that run on top of it.
 */

#include "seaweed.h"
#include "yunix.h"
#include "user.h"
#include "shell.h"

void yunix_main(void)
{
    user_init();
    user_load();   /* keeps the defaults if there is no ys.conf yet */

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_write("\nYunix v" YUNIX_VERSION "\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_write("A hobby operating system by Yavuz Selim\n");
    vga_write("\nType help for a list of commands.\n\n");

    shell_run();   /* never returns */
}
