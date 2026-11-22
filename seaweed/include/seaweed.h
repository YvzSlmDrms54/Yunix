#ifndef SEAWEED_H
#define SEAWEED_H

/* seaweed.h - everything the Seaweed kernel offers to the code above it.
 *
 * This is the one door between the kernel and Yunix. Code under yunix/
 * may include this file and string.h, and nothing else from seaweed/:
 * the Makefile does not even put the kernel's internal headers on its
 * include path, so trying is a compile error.
 *
 * Today Yunix is linked into the same binary and these are plain
 * function calls. When Seaweed learns about ring 3 and system calls,
 * this header is what turns into the system call layer - the code in
 * yunix/ should not have to change.
 *
 * Kernel-only functions (the *_init calls, raw disk access, port I/O)
 * live in seaweed/include/internal/ and are not part of this door.
 */

#include <stddef.h>
#include <stdint.h>

#define SEAWEED_VERSION "0.1"

/* --- the entry point Yunix provides ---
 *
 * Seaweed calls this once every driver is up and the filesystem has been
 * loaded. It must never return. Think of it as init. */
void yunix_main(void);


/* --- screen --- */

/* Text mode is 80 columns by 25 rows. */
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

enum vga_color {
    VGA_BLACK = 0,      VGA_BLUE = 1,         VGA_GREEN = 2,        VGA_CYAN = 3,
    VGA_RED = 4,        VGA_MAGENTA = 5,      VGA_BROWN = 6,        VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,  VGA_LIGHT_BLUE = 9,   VGA_LIGHT_GREEN = 10, VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12, VGA_PINK = 13,        VGA_YELLOW = 14,      VGA_WHITE = 15
};

void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_clear(void);
void vga_putchar(char c);
void vga_write(const char *s);

/* Cursor movement, used by the arrow keys. */
void vga_cursor_left(void);
void vga_cursor_right(void);
void vga_cursor_up(void);
void vga_cursor_down(void);

/* For full-screen programs. vga_put_at paints one cell without moving
 * the cursor or scrolling; cells off the screen are ignored.
 * vga_move_cursor puts the cursor (and the next vga_putchar) at x, y. */
void vga_put_at(size_t x, size_t y, char c, enum vga_color fg, enum vga_color bg);
void vga_move_cursor(size_t x, size_t y);


/* --- keyboard --- */

/* Keys that have no ASCII character get a code above 255, so they can
 * never be confused with a real one. */
#define KEY_UP     0x100
#define KEY_DOWN   0x101
#define KEY_LEFT   0x102
#define KEY_RIGHT  0x103
#define KEY_HOME   0x104
#define KEY_END    0x105
#define KEY_DELETE 0x106
#define KEY_PGUP   0x107
#define KEY_PGDN   0x108

/* Returns the next key: an ASCII character, or one of the KEY_* codes
 * above, or 0 when nothing is waiting. Never blocks. */
int keyboard_getchar(void);


/* --- time --- */

/* How many times per second the timer fires. */
#define TIMER_HZ 100

/* Number of ticks since boot. At 100 Hz this wraps after ~497 days. */
uint32_t timer_ticks(void);

/* Whole seconds since boot. */
uint32_t timer_seconds(void);

/* Waits for the given number of milliseconds. Sleeps between ticks
 * instead of spinning, so the CPU stays cool. */
void timer_sleep(uint32_t ms);


/* --- power --- */

/* Waits until the next interrupt. Call it in a loop that has nothing to
 * do, instead of spinning. */
void power_idle(void);

/* Neither of these returns. */
void power_reboot(void);
void power_halt(void);


/* --- memory --- */

/* Returns a block of at least 'size' bytes, or NULL if there is no room.
 * Always check the result - there is nobody else to catch it. */
void *kmalloc(size_t size);

/* Same, but the block is filled with zeros. */
void *kcalloc(size_t count, size_t size);

/* Gives a block back. Passing NULL is allowed and does nothing.
 * Freeing the same block twice corrupts the heap. */
void  kfree(void *pointer);

/* For a 'mem' command: how much is in use and how much is left. */
void  heap_stats(size_t *used_out, size_t *free_out, size_t *blocks_out);


/* --- disk --- */

/* 1 if a disk was found on the primary IDE channel at boot, 0 if not.
 * Reading and writing sectors is the kernel's business; go through Fish. */
int ata_present(void);

/* What is on the disk, as far as Fish is concerned. Seaweed reads the
 * partition table (MBR or GPT) at boot and only ever writes inside a
 * partition marked as Fish's. */
enum disk_layout {
    DISK_NONE,       /* no disk attached */
    DISK_BLANK,      /* nothing on it at all - fish_format() may partition it */
    DISK_FISH,       /* holds a Fish partition, and Fish lives there */
    DISK_FOREIGN     /* holds other partitions or data and no Fish partition:
                        Seaweed will not write to it */
};

struct disk_info {
    enum disk_layout layout;
    uint32_t    sectors;        /* size of the whole disk */
    const char *table;          /* "MBR", "GPT", "none", ... */
    int         partitions;     /* how many entries the table lists */
    uint32_t    fish_start;     /* the Fish partition, if layout is DISK_FISH */
    uint32_t    fish_sectors;
    int         understood;     /* 1 if every entry made sense, so free space
                                   can be worked out and a partition added */
};

void disk_info(struct disk_info *out);

/* One partition from the table, and one stretch of free space. */
struct disk_partition {
    uint32_t    start;
    uint32_t    sectors;
    const char *kind;           /* "Linux filesystem", "Fish", "other", ... */
};

struct disk_region {
    uint32_t start;             /* always on a 1 MiB boundary */
    uint32_t sectors;           /* at least 1 MiB */
};

/* Fill 'out' with up to 'max' entries and return how many there are. */
int disk_partitions(struct disk_partition *out, int max);
int disk_free_space(struct disk_region *out, int max);

/* Adds a Fish partition from 'start' for 'sectors' sectors. The whole of
 * it must lie inside free space, as disk_free_space() reports it: every
 * existing partition and all their data stay exactly as they were. The
 * only bytes written are one new entry in the partition table (and, on
 * GPT, its checksums and the backup copy at the end of the disk).
 * Returns DISK_OK or one of the DISK_ERR_* codes. */
int disk_create_fish(uint32_t start, uint32_t sectors);

#define DISK_OK               0
#define DISK_ERR_NODISK      -1
#define DISK_ERR_HAS_FISH    -2   /* there is a Fish partition already */
#define DISK_ERR_NOT_FREE    -3   /* not inside free space */
#define DISK_ERR_TABLE_FULL  -4   /* all four MBR slots are taken */
#define DISK_ERR_DAMAGED     -5   /* the table does not add up */
#define DISK_ERR_UNSUPPORTED -6   /* data but no table, or out of reach */
#define DISK_ERR_IO          -7
#define DISK_ERR_TOO_SMALL   -8   /* under 1 MiB */

const char *disk_error(int code);


/* --- Fish, the filesystem ---
 *
 * The tree lives in RAM and is written to disk in one piece by
 * fish_save(), then read back by fish_load(). Nothing is saved
 * automatically: changes are lost unless you save them.
 *
 * The node pool is a fixed size decided at compile time. Nodes refer to
 * their parent by index rather than by pointer, which is what lets the
 * whole array go to disk unchanged.
 */

#define FISH_NAME_MAX   32
#define FISH_MAX_NODES  64
#define FISH_FILE_MAX   4096
#define FISH_PATH_MAX   256

/* Return codes. Zero is success, negatives are errors, so callers can
 * write "if (fish_mkdir(name) < 0)". */
#define FISH_OK          0
#define FISH_ERR_EXISTS -1
#define FISH_ERR_NOTDIR -2
#define FISH_ERR_NOENT  -3
#define FISH_ERR_FULL   -4
#define FISH_ERR_NAME   -5
#define FISH_ERR_ISDIR  -6
#define FISH_ERR_NOTEMPTY -7
#define FISH_ERR_SPACE  -8
#define FISH_ERR_NODISK -9
#define FISH_ERR_IO     -10
#define FISH_ERR_FORMAT -11
#define FISH_ERR_NOMEM  -12
#define FISH_ERR_NOPART -13   /* the disk is blank: no Fish partition yet */
#define FISH_ERR_FOREIGN -14  /* the disk belongs to something else */
#define FISH_ERR_SMALL  -15   /* the Fish partition cannot hold the image */

/* Empties the filesystem in memory, leaving only the root. */
void fish_init(void);

/* Directory operations, all relative to the current directory. */
int  fish_mkdir(const char *name);
int  fish_chdir(const char *name);   /* ".." goes up, "/" goes to the root */
int  fish_remove(const char *name);

/* File operations. */
int  fish_create(const char *name);
int  fish_write(const char *name, const char *text);   /* replaces contents */
int  fish_append(const char *name, const char *text);
const char *fish_read(const char *name, size_t *size_out);

/* Listing. Call fish_first() then fish_next() until it returns 0.
 * 'is_dir_out' may be NULL if you do not care. */
int  fish_first(void);
int  fish_next(int handle, const char **name_out, int *is_dir_out,
               size_t *size_out);

/* Remembers the current directory so you can move away and come back.
 * The handle is only meaningful to Fish - do not interpret it. */
int  fish_save_cwd(void);
void fish_restore_cwd(int handle);

/* Writes the current directory as a path like "/notes/2026". */
void fish_path(char *buffer, size_t size);

/* Writes the whole filesystem to its partition, or reads it back. Both
 * need a disk with a Fish partition; without one they fail cleanly. */
int  fish_save(void);
int  fish_load(void);

/* Erases the filesystem, in memory and on the disk. On a blank disk it
 * first creates a partition table with one Fish partition. On a disk that
 * holds anything else it refuses (FISH_ERR_FOREIGN) and changes nothing. */
int  fish_format(void);

/* Turns an error code into something a human can read. */
const char *fish_error(int code);

#endif
