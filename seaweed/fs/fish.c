/* seaweed/fs/fish.c - the Fish filesystem, version 1.1
 *
 * A tree of nodes held in one fixed array. Each node knows its parent by
 * index rather than by pointer, which keeps the whole thing relocatable
 * and easy to write out to a disk later.
 *
 * Node 0 is always the root and is never freed.
 */

#include "seaweed.h"
#include "string.h"
#include "ata.h"
#include "heap.h"
#include <stdint.h>

struct node {
    char   name[FISH_NAME_MAX];
    int    used;
    int    is_dir;
    int    parent;                 /* index of the parent, -1 for the root */
    size_t size;                   /* bytes used in data, files only */
    char   data[FISH_FILE_MAX];
};

static struct node nodes[FISH_MAX_NODES];
static int current;                /* index of the current directory */

/* --- helpers --- */

/* Names may not be empty, too long, or contain a slash or a space. */
static int valid_name(const char *name)
{
    size_t i;

    if (name == 0 || name[0] == '\0') return 0;

    for (i = 0; name[i] != '\0'; i++) {
        if (i >= FISH_NAME_MAX - 1) return 0;
        if (name[i] == '/' || name[i] <= ' ') return 0;
    }

    /* "." and ".." are reserved for navigation. */
    if (strcmp(name, ".") == 0)  return 0;
    if (strcmp(name, "..") == 0) return 0;

    return 1;
}

/* Finds a child of 'parent' by name. Returns its index, or -1. */
static int find_child(int parent, const char *name)
{
    for (int i = 0; i < FISH_MAX_NODES; i++) {
        if (!nodes[i].used) continue;
        if (nodes[i].parent != parent) continue;
        if (strcmp(nodes[i].name, name) == 0) return i;
    }
    return -1;
}

/* Takes an unused slot from the pool. Returns its index, or -1 if the
 * filesystem is full. */
static int allocate(void)
{
    for (int i = 1; i < FISH_MAX_NODES; i++) {   /* 0 is the root */
        if (!nodes[i].used) return i;
    }
    return -1;
}

/* Creates a child of the current directory. */
static int make_node(const char *name, int is_dir)
{
    int index;

    if (!valid_name(name))                    return FISH_ERR_NAME;
    if (find_child(current, name) >= 0)       return FISH_ERR_EXISTS;

    index = allocate();
    if (index < 0)                            return FISH_ERR_FULL;

    strcpy(nodes[index].name, name);
    nodes[index].used   = 1;
    nodes[index].is_dir = is_dir;
    nodes[index].parent = current;
    nodes[index].size   = 0;

    return index;
}

/* --- public API --- */

void fish_init(void)
{
    memset(nodes, 0, sizeof(nodes));

    strcpy(nodes[0].name, "/");
    nodes[0].used   = 1;
    nodes[0].is_dir = 1;
    nodes[0].parent = -1;
    nodes[0].size   = 0;

    current = 0;
}

int fish_mkdir(const char *name)
{
    int result = make_node(name, 1);
    return result < 0 ? result : FISH_OK;
}

int fish_create(const char *name)
{
    int result = make_node(name, 0);
    return result < 0 ? result : FISH_OK;
}

int fish_chdir(const char *name)
{
    int index;

    if (name == 0 || name[0] == '\0') return FISH_ERR_NOENT;

    if (strcmp(name, "/") == 0)  { current = 0; return FISH_OK; }
    if (strcmp(name, ".") == 0)  { return FISH_OK; }

    if (strcmp(name, "..") == 0) {
        /* The root's parent is itself, as far as the user is concerned. */
        if (nodes[current].parent >= 0) current = nodes[current].parent;
        return FISH_OK;
    }

    index = find_child(current, name);
    if (index < 0)            return FISH_ERR_NOENT;
    if (!nodes[index].is_dir) return FISH_ERR_NOTDIR;

    current = index;
    return FISH_OK;
}

int fish_remove(const char *name)
{
    int index = find_child(current, name);

    if (index < 0) return FISH_ERR_NOENT;

    /* Refuse to delete a directory that still holds something. Deleting
     * it anyway would strand its children: they would still be marked
     * used, but nothing could reach them. */
    if (nodes[index].is_dir) {
        for (int i = 0; i < FISH_MAX_NODES; i++) {
            if (nodes[i].used && nodes[i].parent == index) {
                return FISH_ERR_NOTEMPTY;
            }
        }
    }

    nodes[index].used = 0;
    return FISH_OK;
}

int fish_write(const char *name, const char *text)
{
    int index = find_child(current, name);
    size_t length;

    if (index < 0)           return FISH_ERR_NOENT;
    if (nodes[index].is_dir) return FISH_ERR_ISDIR;

    length = strlen(text);
    if (length > FISH_FILE_MAX) return FISH_ERR_SPACE;

    memcpy(nodes[index].data, text, length);
    nodes[index].size = length;
    return FISH_OK;
}

int fish_append(const char *name, const char *text)
{
    int index = find_child(current, name);
    size_t length;

    if (index < 0)           return FISH_ERR_NOENT;
    if (nodes[index].is_dir) return FISH_ERR_ISDIR;

    length = strlen(text);
    if (nodes[index].size + length > FISH_FILE_MAX) return FISH_ERR_SPACE;

    memcpy(nodes[index].data + nodes[index].size, text, length);
    nodes[index].size += length;
    return FISH_OK;
}

const char *fish_read(const char *name, size_t *size_out)
{
    int index = find_child(current, name);

    if (index < 0 || nodes[index].is_dir) return 0;

    if (size_out != 0) *size_out = nodes[index].size;
    return nodes[index].data;
}

/* Listing works with a plain integer cursor: fish_first() gives you a
 * starting point, fish_next() hands back one entry and moves it along. */
int fish_first(void)
{
    return 0;
}

int fish_next(int handle, const char **name_out, int *is_dir_out,
              size_t *size_out)
{
    for (int i = handle; i < FISH_MAX_NODES; i++) {
        if (!nodes[i].used) continue;
        if (nodes[i].parent != current) continue;

        if (name_out   != 0) *name_out   = nodes[i].name;
        if (is_dir_out != 0) *is_dir_out = nodes[i].is_dir;
        if (size_out   != 0) *size_out   = nodes[i].size;

        return i + 1;      /* where to carry on from next time */
    }
    return 0;              /* nothing left */
}

int fish_save_cwd(void)
{
    return current;
}

void fish_restore_cwd(int handle)
{
    if (handle >= 0 && handle < FISH_MAX_NODES &&
        nodes[handle].used && nodes[handle].is_dir) {
        current = handle;
    } else {
        current = 0;
    }
}

void fish_path(char *buffer, size_t size)
{
    /* Walk from the current directory up to the root, collecting the
     * indices, then write them out backwards. */
    int chain[FISH_MAX_NODES];
    int depth = 0;
    int node = current;
    size_t pos = 0;

    if (size == 0) return;

    while (node > 0 && depth < FISH_MAX_NODES) {
        chain[depth++] = node;
        node = nodes[node].parent;
    }

    if (depth == 0) {                      /* we are at the root */
        if (size >= 2) { buffer[0] = '/'; buffer[1] = '\0'; }
        else buffer[0] = '\0';
        return;
    }

    for (int i = depth - 1; i >= 0; i--) {
        const char *name = nodes[chain[i]].name;

        if (pos + 1 < size) buffer[pos++] = '/';

        for (size_t j = 0; name[j] != '\0'; j++) {
            if (pos + 1 < size) buffer[pos++] = name[j];
        }
    }

    buffer[pos] = '\0';
}

/* --- saving and loading --- */

/* "FISH" as four bytes, read back as a little-endian number. */
#define FISH_MAGIC   0x48534946u
#define FISH_VERSION 1u

/* The first sector of our area holds this; the nodes follow it. */
struct superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t node_count;   /* how many nodes the image was written with */
    uint32_t node_size;    /* and how big each one was */
    uint32_t current;      /* which directory was open at save time */
};

/* How many whole sectors the node array needs. */
#define NODE_BYTES   (sizeof(nodes))
#define NODE_SECTORS ((NODE_BYTES + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE)
/* Where our image starts on the disk.
 *
 * Not sector 0: that is the MBR, which holds the partition table and the
 * first stage of the machine's boot loader. Overwriting it on a real
 * computer would stop the installed system from starting. */
#define FISH_START_LBA 2048

int fish_save(void)
{
    char sector[ATA_SECTOR_SIZE];
    struct superblock sb;
    const char *source = (const char *)nodes;
    uint32_t written = 0;

    if (!ata_present()) return FISH_ERR_NODISK;

    sb.magic      = FISH_MAGIC;
    sb.version    = FISH_VERSION;
    sb.node_count = FISH_MAX_NODES;
    sb.node_size  = (uint32_t)sizeof(struct node);
    sb.current    = (uint32_t)current;

    /* Wipe the superblock first and write it again last. If a write fails
     * in between, the disk holds no valid image - which fish_load reports
     * cleanly - instead of a valid-looking header in front of half-written
     * nodes, which it would happily believe. */
    memset(sector, 0, sizeof(sector));
    if (ata_write(FISH_START_LBA, 1, sector) != 0) return FISH_ERR_IO;

    /* Copy through a 512-byte buffer so the last, partly filled sector
     * is padded with zeros rather than with whatever follows in memory. */
    for (uint32_t s = 0; s < NODE_SECTORS; s++) {
        uint32_t remaining = (uint32_t)NODE_BYTES - written;
        uint32_t chunk = remaining < ATA_SECTOR_SIZE ? remaining
                                                     : ATA_SECTOR_SIZE;

        memset(sector, 0, sizeof(sector));
        memcpy(sector, source + written, chunk);

        if (ata_write(FISH_START_LBA + 1 + s, 1, sector) != 0) return FISH_ERR_IO;
        written += chunk;
    }

    /* Every node is on the disk. Only now does the image become valid. */
    memset(sector, 0, sizeof(sector));
    memcpy(sector, &sb, sizeof(sb));
    if (ata_write(FISH_START_LBA, 1, sector) != 0) return FISH_ERR_IO;

    return FISH_OK;
}

/* Checks a node array that came off the disk before we trust it. A
 * damaged image must not be able to crash the kernel: an unterminated
 * name would run strcmp off the end, a size above FISH_FILE_MAX would
 * make cat read past the node, and a parent index outside the pool would
 * send fish_path into random memory. Returns 1 if the image is sane. */
static int image_is_sane(const struct node *image)
{
    const struct node *root = &image[0];

    if (root->used != 1 || root->is_dir != 1 || root->parent != -1) return 0;

    for (int i = 1; i < FISH_MAX_NODES; i++) {
        const struct node *n = &image[i];
        int terminated = 0;
        int up, steps;

        if (n->used == 0) continue;
        if (n->used != 1) return 0;
        if (n->is_dir != 0 && n->is_dir != 1) return 0;
        if (n->size > FISH_FILE_MAX) return 0;

        for (int j = 0; j < FISH_NAME_MAX; j++) {
            if (n->name[j] == '\0') { terminated = 1; break; }
        }
        if (!terminated || !valid_name(n->name)) return 0;

        if (n->parent < 0 || n->parent >= FISH_MAX_NODES) return 0;
        if (image[n->parent].used != 1 || image[n->parent].is_dir != 1) return 0;

        /* Every node must lead back to the root. Without this, two nodes
         * naming each other as parent would form a loop nobody can reach. */
        up = i;
        for (steps = 0; up != 0 && steps < FISH_MAX_NODES; steps++) {
            up = image[up].parent;
        }
        if (up != 0) return 0;
    }

    return 1;
}

int fish_load(void)
{
    char sector[ATA_SECTOR_SIZE];
    struct superblock sb;
    struct node *image;
    char *dest;
    uint32_t read_so_far = 0;

    if (!ata_present()) return FISH_ERR_NODISK;

    if (ata_read(FISH_START_LBA, 1, sector) != 0) return FISH_ERR_IO;
    memcpy(&sb, sector, sizeof(sb));

    /* Refuse anything we do not recognise. Loading a mismatched image
     * would fill the node array with nonsense. */
    if (sb.magic != FISH_MAGIC)                    return FISH_ERR_FORMAT;
    if (sb.version != FISH_VERSION)                return FISH_ERR_FORMAT;
    if (sb.node_count != FISH_MAX_NODES)           return FISH_ERR_FORMAT;
    if (sb.node_size != sizeof(struct node))       return FISH_ERR_FORMAT;

    /* Read into a scratch copy, not straight into 'nodes'. If the disk
     * fails halfway, or the image turns out to be damaged, the files in
     * memory are left exactly as they were. */
    image = kmalloc(NODE_BYTES);
    if (image == 0) return FISH_ERR_NOMEM;
    dest = (char *)image;

    for (uint32_t s = 0; s < NODE_SECTORS; s++) {
        uint32_t remaining = (uint32_t)NODE_BYTES - read_so_far;
        uint32_t chunk = remaining < ATA_SECTOR_SIZE ? remaining
                                                     : ATA_SECTOR_SIZE;

        if (ata_read(FISH_START_LBA + 1 + s, 1, sector) != 0) {
            kfree(image);
            return FISH_ERR_IO;
        }

        memcpy(dest + read_so_far, sector, chunk);
        read_so_far += chunk;
    }

    if (!image_is_sane(image)) {
        kfree(image);
        return FISH_ERR_FORMAT;
    }

    memcpy(nodes, image, NODE_BYTES);
    kfree(image);

    /* The saved directory index came off a disk, so treat it as
     * untrusted: fall back to the root if it makes no sense. */
    if (sb.current >= FISH_MAX_NODES ||
        !nodes[sb.current].used ||
        !nodes[sb.current].is_dir) {
        current = 0;
    } else {
        current = (int)sb.current;
    }

    return FISH_OK;
}

const char *fish_error(int code)
{
    switch (code) {
    case FISH_OK:            return "ok";
    case FISH_ERR_EXISTS:    return "already exists";
    case FISH_ERR_NOTDIR:    return "not a directory";
    case FISH_ERR_NOENT:     return "no such file or directory";
    case FISH_ERR_FULL:      return "filesystem is full";
    case FISH_ERR_NAME:      return "bad name";
    case FISH_ERR_ISDIR:     return "is a directory";
    case FISH_ERR_NOTEMPTY:  return "directory is not empty";
    case FISH_ERR_SPACE:     return "file is too big";
    case FISH_ERR_NODISK:    return "no disk found";
    case FISH_ERR_IO:        return "disk error";
    case FISH_ERR_FORMAT:    return "disk holds no valid Fish image";
    case FISH_ERR_NOMEM:     return "out of memory";
    default:                 return "unknown error";
    }
}