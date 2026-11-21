#ifndef HEAP_H
#define HEAP_H

/* A first-fit heap over one fixed arena.
 *
 * This is not a physical memory manager: there is no paging yet, so the
 * arena is simply a large static array. When Seaweed learns about paging
 * this file is what changes, and everything calling kmalloc stays as it
 * is.
 *
 * kmalloc, kcalloc, kfree and heap_stats are declared in seaweed.h. */

#include "seaweed.h"

void heap_init(void);

#endif
