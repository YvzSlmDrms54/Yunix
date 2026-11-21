#ifndef TIMER_H
#define TIMER_H

/* Kernel side of the PIT driver. TIMER_HZ and the clock calls are in
 * seaweed.h. */

#include "seaweed.h"

void timer_init(void);

#endif
