#ifndef KEYBOARD_H
#define KEYBOARD_H

/* Kernel side of the keyboard driver. KEY_* codes and keyboard_getchar()
 * are in seaweed.h. */

#include "seaweed.h"

void keyboard_init(void);

#endif
