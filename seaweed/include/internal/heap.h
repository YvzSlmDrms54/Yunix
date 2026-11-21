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

/* Size of the arena. It has to hold more than one copy of the Fish node
 * pool, because fish_load() reads the disk into a scratch copy first. */
#define HEAP_SIZE (1024 * 1024)

void heap_init(void);

#endif
