#ifndef VGA_H
#define VGA_H

/* Kernel side of the screen driver. The drawing calls Yunix may use are
 * in seaweed.h; only setting the driver up is kept in here. */

#include "seaweed.h"

void vga_init(void);

#endif
