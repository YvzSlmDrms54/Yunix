/* seaweed/drivers/ahci.c - SATA disks through an AHCI controller.
 *
 * AHCI is how a PC talks to SATA disks today. Instead of the old IDE
 * driver's I/O ports, the controller is a block of memory-mapped
 * registers (its "ABAR", found in PCI BAR5):
 *
 *     0x000  generic registers: capabilities, global control, which
 *            ports exist, version
 *     0x100  one 128-byte block per port (up to 32), each with
 *            its own status, signature and command registers
 *
 * Commands are not written to registers one by one. For each port, the
 * kernel sets up three structures in ordinary memory and tells the port
 * where they are:
 *
 *     command list   32 slots of 32 bytes; a slot says where its command
 *                    table is and how long the command FIS is
 *     command table  the command itself (a "FIS" - frame information
 *                    structure, the packet SATA sends), followed by a
 *                    list of memory regions to move data to or from
 *     received FIS   where the port puts the disk's replies
 *
 * Then setting a bit in the port's "command issue" register starts it,
 * and the controller moves the data itself (DMA) while the CPU waits.
 *
 * This first part only finds what is plugged in and asks each device
 * who it is (IDENTIFY). Reading and writing come next.
 */

#include "ahci.h"
#include "pci.h"
#include "paging.h"
#include "string.h"

/* Generic registers, as offsets in 32-bit words. */
#define HBA_CAP        (0x00 / 4)
#define HBA_GHC        (0x04 / 4)
#define HBA_IS         (0x08 / 4)
#define HBA_PI         (0x0C / 4)
#define HBA_VS         (0x10 / 4)
#define HBA_CAP2       (0x24 / 4)
#define HBA_BOHC       (0x28 / 4)

#define CAP_SSS        (1u << 27)     /* staggered spin-up: ports start off */
#define GHC_AE         (1u << 31)     /* AHCI enable */
#define GHC_IE         (1u << 1)      /* interrupts - we poll instead */
#define CAP2_BOH       (1u << 0)      /* BIOS/OS handoff is supported */
#define BOHC_BOS       (1u << 0)      /* BIOS owns the controller */
#define BOHC_OOS       (1u << 1)      /* OS wants to own it */
#define BOHC_BB        (1u << 4)      /* BIOS is busy finishing up */

/* Per-port registers, as offsets in 32-bit words. */
#define PORT_CLB       (0x00 / 4)     /* command list address */
#define PORT_CLBU      (0x04 / 4)
#define PORT_FB        (0x08 / 4)     /* received FIS address */
#define PORT_FBU       (0x0C / 4)
#define PORT_IS        (0x10 / 4)     /* interrupt status */
#define PORT_IE        (0x14 / 4)
#define PORT_CMD       (0x18 / 4)
#define PORT_TFD       (0x20 / 4)     /* the device's status and error */
#define PORT_SIG       (0x24 / 4)     /* what kind of device this is */
#define PORT_SSTS      (0x28 / 4)     /* link status */
#define PORT_SCTL      (0x2C / 4)     /* link control: used for COMRESET */
#define PORT_SERR      (0x30 / 4)
#define PORT_CI        (0x38 / 4)     /* command issue: one bit per slot */

#define CMD_ST         (1u << 0)      /* start processing the command list */
#define CMD_SUD        (1u << 1)      /* spin up the device */
#define CMD_FRE        (1u << 4)      /* accept received FISes */
#define CMD_FR         (1u << 14)     /* FIS receiving is running */
#define CMD_CR         (1u << 15)     /* command list is running */

#define TFD_ERR        (1u << 0)
#define TFD_DRQ        (1u << 3)
#define TFD_BSY        (1u << 7)

#define IS_TFES        (1u << 30)     /* the device reported an error */

#define SSTS_DET_MASK  0x0F
#define SSTS_DET_UP    0x03           /* a device is there and talking */

/* What PxSIG says once the device has introduced itself. */
#define SIG_ATA        0x00000101u    /* a disk */
#define SIG_ATAPI      0xEB140101u    /* a CD/DVD drive */
#define SIG_SEMB       0xC33C0101u    /* an enclosure bridge */
#define SIG_PM         0x96690101u    /* a port multiplier */

#define FIS_REG_H2D    0x27           /* a command from us to the device */

#define ATA_IDENTIFY         0xEC
#define ATA_IDENTIFY_PACKET  0xA1
#define ATA_READ_DMA         0xC8   /* 28-bit */
#define ATA_WRITE_DMA        0xCA
#define ATA_FLUSH            0xE7
#define ATA_READ_DMA_EXT     0x25   /* 48-bit */
#define ATA_WRITE_DMA_EXT    0x35
#define ATA_FLUSH_EXT        0xEA

#define MAX_PORTS      32

/* The three structures for one port. The controller wants the command
 * list on a 1 KiB boundary, the received-FIS area on 256 bytes and the
 * command table on 128 - aligning the whole thing to 1 KiB and laying
 * them out in this order satisfies all three.
 *
 * The controller is given these addresses as physical addresses. That
 * works because the kernel is identity-mapped: its addresses are the
 * physical ones. */
struct port_memory {
    uint8_t command_list[1024];
    uint8_t received_fis[256];
    uint8_t command_table[256];   /* 128 bytes of FIS area, then PRD entries */
} __attribute__((aligned(1024)));

static struct port_memory memory[MAX_PORTS];
static uint16_t           identify_buffer[256] __attribute__((aligned(16)));

static volatile uint32_t *hba;
static uint32_t           version;
static struct ahci_device devices[AHCI_MAX_DEVICES];
static int                device_count;

/* --- waiting, always with a time limit --- */

/* Waits until (register & mask) == want, for at most 'ms' milliseconds.
 * Returns 0 if it got there, -1 on timeout. */
static int wait_for(volatile uint32_t *reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint32_t deadline = timer_ticks() + ms / (1000 / TIMER_HZ) + 1;

    while ((*reg & mask) != want) {
        if ((int32_t)(timer_ticks() - deadline) > 0) return -1;
    }
    return 0;
}

static volatile uint32_t *port_regs(int port)
{
    return hba + (0x100 + port * 0x80) / 4;
}

/* --- taking the controller over --- */

/* On real machines the firmware may still be using the controller. The
 * handoff protocol asks it to let go; QEMU does not need it, but real
 * boards do. */
static void bios_handoff(void)
{
    if (!(hba[HBA_CAP2] & CAP2_BOH)) return;

    hba[HBA_BOHC] |= BOHC_OOS;
    wait_for(&hba[HBA_BOHC], BOHC_BOS, 0, 25);
    if (hba[HBA_BOHC] & BOHC_BB) wait_for(&hba[HBA_BOHC], BOHC_BB, 0, 2000);
}

/* A port must be idle before its structures may be changed. */
static int port_stop(volatile uint32_t *p)
{
    p[PORT_CMD] &= ~CMD_ST;
    if (wait_for(&p[PORT_CMD], CMD_CR, 0, 500) != 0) return -1;
    p[PORT_CMD] &= ~CMD_FRE;
    if (wait_for(&p[PORT_CMD], CMD_FR, 0, 500) != 0) return -1;
    return 0;
}

static int port_start(int port)
{
    volatile uint32_t *p = port_regs(port);
    struct port_memory *m = &memory[port];

    if (port_stop(p) != 0) return -1;

    memset(m, 0, sizeof(*m));
    p[PORT_CLB]  = (uint32_t)m->command_list;
    p[PORT_CLBU] = 0;
    p[PORT_FB]   = (uint32_t)m->received_fis;
    p[PORT_FBU]  = 0;

    p[PORT_SERR] = 0xFFFFFFFFu;     /* these clear by writing 1 */
    p[PORT_IS]   = 0xFFFFFFFFu;
    p[PORT_IE]   = 0;               /* we poll */

    p[PORT_CMD] |= CMD_FRE;
    if (wait_for(&p[PORT_TFD], TFD_BSY | TFD_DRQ, 0, 1000) != 0) return -1;
    p[PORT_CMD] |= CMD_ST;
    return 0;
}

/* --- one command --- */

/* After an error or a timeout the port's command engine stops, and the
 * spec's way back is to stop the port, clear its error bits and start it
 * again. If the device itself is wedged (still busy), a COMRESET - a
 * reset sent down the SATA link - comes first. */
static void port_recover(int port)
{
    volatile uint32_t *p = port_regs(port);

    port_stop(p);

    if (p[PORT_TFD] & (TFD_BSY | TFD_DRQ)) {
        p[PORT_SCTL] = (p[PORT_SCTL] & ~0xFu) | 0x1;      /* DET = 1: reset */
        timer_sleep(10);                                  /* spec: at least 1 ms */
        p[PORT_SCTL] &= ~0xFu;                            /* DET = 0: let go */
        wait_for(&p[PORT_SSTS], SSTS_DET_MASK, SSTS_DET_UP, 1000);
    }

    p[PORT_SERR] = 0xFFFFFFFFu;
    p[PORT_IS]   = 0xFFFFFFFFu;
    port_start(port);
}

/* Sends one command through slot 0 and waits for it to finish.
 *
 *   fis        the 20-byte command FIS
 *   writing    1 if data goes from memory to the device
 *   buffer     where the data goes or comes from (NULL with bytes = 0)
 *   bytes      how much; at most 4 MiB, which is what one PRD entry holds
 *
 * Returns AHCI_OK, AHCI_ERR_DEVICE if the device reported an error, or
 * AHCI_ERR_TIMEOUT. After either error the port is recovered, so the
 * next command has a fair chance. */
static int issue(int port, const uint8_t *fis, int writing,
                 void *buffer, uint32_t bytes, uint32_t timeout_ms)
{
    volatile uint32_t *p = port_regs(port);
    struct port_memory *m = &memory[port];
    uint32_t *header = (uint32_t *)m->command_list;
    uint32_t *prd = (uint32_t *)(m->command_table + 0x80);
    uint32_t deadline;
    int result = AHCI_OK;

    if (wait_for(&p[PORT_TFD], TFD_BSY | TFD_DRQ, 0, 1000) != 0) {
        port_recover(port);
        return AHCI_ERR_TIMEOUT;
    }

    /* Slot 0's header: the command FIS is 5 dwords long, the write bit
     * says which way data flows, and there is one PRD entry - or none,
     * for a command that moves no data. */
    header[0] = 5u | (writing ? (1u << 6) : 0) | ((bytes > 0 ? 1u : 0u) << 16);
    header[1] = 0;                              /* bytes moved, filled in by the HBA */
    header[2] = (uint32_t)m->command_table;
    header[3] = 0;

    memset(m->command_table, 0, sizeof(m->command_table));
    memcpy(m->command_table, fis, 20);

    if (bytes > 0) {
        /* One region of memory. The count field holds the length minus
         * one, and the address must be even. */
        prd[0] = (uint32_t)buffer;
        prd[1] = 0;
        prd[2] = 0;
        prd[3] = bytes - 1;
    }

    p[PORT_IS] = 0xFFFFFFFFu;
    p[PORT_CI] = 1;

    deadline = timer_ticks() + timeout_ms / (1000 / TIMER_HZ) + 1;
    while (p[PORT_CI] & 1) {
        if (p[PORT_IS] & IS_TFES)                     { result = AHCI_ERR_DEVICE;  break; }
        if ((int32_t)(timer_ticks() - deadline) > 0)  { result = AHCI_ERR_TIMEOUT; break; }
    }

    if (result == AHCI_OK && (p[PORT_TFD] & TFD_ERR)) result = AHCI_ERR_DEVICE;
    if (result != AHCI_OK) port_recover(port);
    return result;
}

/* A command FIS for the older commands: IDENTIFY, and 28-bit reads,
 * writes and flushes. */
static void make_fis(uint8_t *fis, uint8_t command)
{
    memset(fis, 0, 20);
    fis[0] = FIS_REG_H2D;
    fis[1] = 0x80;                              /* this FIS carries a command */
    fis[2] = command;
}

/* Fills in the address and count. With 48-bit addressing the address
 * is six bytes and the count two; the 28-bit commands keep the top four
 * address bits in the device register. Bit 6 of that register selects
 * LBA addressing, as opposed to the ancient cylinder/head/sector kind. */
static void set_lba(uint8_t *fis, uint64_t lba, uint16_t count, int lba48)
{
    fis[4]  = (uint8_t)lba;
    fis[5]  = (uint8_t)(lba >> 8);
    fis[6]  = (uint8_t)(lba >> 16);
    fis[7]  = 0x40;
    if (lba48) {
        fis[8]  = (uint8_t)(lba >> 24);
        fis[9]  = (uint8_t)(lba >> 32);
        fis[10] = (uint8_t)(lba >> 40);
    } else {
        fis[7] |= (uint8_t)((lba >> 24) & 0x0F);
    }
    fis[12] = (uint8_t)count;
    fis[13] = (uint8_t)(count >> 8);
}

/* --- making sense of IDENTIFY --- */

/* The model name is words 27-46, two characters per word with the first
 * one in the high byte - an old ATA quirk. Trailing spaces are padding. */
static void read_model(const uint16_t *id, char *out)
{
    int n = 0;

    for (int w = 27; w <= 46; w++) {
        out[n++] = (char)(id[w] >> 8);
        out[n++] = (char)(id[w] & 0xFF);
    }
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0')) out[--n] = '\0';
    for (int i = 0; i < n; i++) if (out[i] < ' ' || out[i] > '~') out[i] = '?';
}

static void add_device(int port, enum ahci_kind kind)
{
    struct ahci_device *d;
    const uint16_t *id = identify_buffer;
    uint8_t command = (kind == AHCI_ATAPI) ? ATA_IDENTIFY_PACKET : ATA_IDENTIFY;

    if (device_count >= AHCI_MAX_DEVICES) return;
    d = &devices[device_count++];

    d->port = port;
    d->kind = kind;
    d->model[0] = '\0';
    d->sectors = 0;
    d->lba48 = 0;
    d->identified = 0;

    if (kind != AHCI_DISK && kind != AHCI_ATAPI) return;
    if (port_start(port) != 0) return;

    memset(identify_buffer, 0, sizeof(identify_buffer));
    {
        uint8_t fis[20];

        make_fis(fis, command);
        if (issue(port, fis, 0, identify_buffer, 512, 2000) != AHCI_OK) return;
    }

    d->identified = 1;
    read_model(id, d->model);

    if (kind == AHCI_DISK) {
        /* Word 83 bit 10: 48-bit addressing. Then the size is words
         * 100-103; without it, the 28-bit size in words 60-61. */
        d->lba48 = (id[83] >> 10) & 1;
        if (d->lba48) {
            d->sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                         ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
        } else {
            d->sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
        }
    }
}

/* --- public --- */

int ahci_init(void)
{
    const struct pci_device *controller = pci_find(0x01, 0x06, 0x01);
    uint32_t abar, implemented;

    hba = 0;
    device_count = 0;
    if (controller == 0) return -1;

    /* BAR5 is the controller's register block: a 32-bit memory BAR. The
     * low four bits are flags, not address. */
    abar = controller->bar[5] & ~0xFu;
    if (abar == 0 || (controller->bar[5] & 0x1)) return -1;

    pci_enable(controller);
    hba = paging_map_device(abar, 0x1100);     /* 0x100 + 32 ports x 0x80 */
    if (hba == 0) return -1;

    bios_handoff();
    hba[HBA_GHC] |= GHC_AE;
    hba[HBA_GHC] &= ~GHC_IE;
    version = hba[HBA_VS];

    implemented = hba[HBA_PI];
    for (int port = 0; port < MAX_PORTS; port++) {
        volatile uint32_t *p;
        uint32_t signature;

        if (!(implemented & (1u << port))) continue;
        p = port_regs(port);

        /* With staggered spin-up, ports start powered down until told. */
        if ((hba[HBA_CAP] & CAP_SSS) && !(p[PORT_CMD] & CMD_SUD)) {
            p[PORT_CMD] |= CMD_SUD;
            wait_for(&p[PORT_SSTS], SSTS_DET_MASK, SSTS_DET_UP, 50);
        }

        if ((p[PORT_SSTS] & SSTS_DET_MASK) != SSTS_DET_UP) continue;   /* empty */

        signature = p[PORT_SIG];
        if      (signature == SIG_ATA)   add_device(port, AHCI_DISK);
        else if (signature == SIG_ATAPI) add_device(port, AHCI_ATAPI);
        else                             add_device(port, AHCI_OTHER);
    }

    return device_count;
}

int ahci_present(void)
{
    return hba != 0;
}

uint32_t ahci_version(void)
{
    return version;
}

int ahci_count(void)
{
    return device_count;
}

const struct ahci_device *ahci_get(int index)
{
    if (index < 0 || index >= device_count) return 0;
    return &devices[index];
}

/* --- reading and writing ---
 *
 * Big transfers are split into commands of at most CHUNK_SECTORS. One
 * PRD entry could describe up to 4 MiB, but 64 KiB keeps each command
 * well inside the timeout on slow disks and inside what the 28-bit
 * commands can count. */

#define CHUNK_SECTORS   128
#define SECTOR_BYTES    512
#define RW_TIMEOUT_MS   5000
#define FLUSH_TIMEOUT_MS 30000      /* a real disk may take a while */

/* Only identity-mapped RAM can be handed to the controller: there,
 * the address the kernel uses is the physical address the device needs.
 * Device memory and anything past the map would be nonsense to it. */
#define DMA_LIMIT       (16u * 1024 * 1024)

static int check_request(int index, uint64_t lba, uint32_t count, const void *buffer,
                         const struct ahci_device **out)
{
    const struct ahci_device *d = ahci_get(index);
    uint32_t address = (uint32_t)buffer;
    uint64_t limit;

    if (d == 0 || d->kind != AHCI_DISK || !d->identified) return AHCI_ERR_NODEV;
    if (count == 0) return AHCI_OK;

    /* The device's own size, and for 28-bit disks the command's reach. */
    limit = d->sectors;
    if (!d->lba48 && limit > 0x10000000u) limit = 0x10000000u;
    if (lba >= limit || count > limit - lba) return AHCI_ERR_RANGE;

    if (address == 0 || (address & 1)) return AHCI_ERR_BUFFER;
    if (count > (DMA_LIMIT - address) / SECTOR_BYTES || address >= DMA_LIMIT) return AHCI_ERR_BUFFER;

    *out = d;
    return AHCI_OK;
}

static int transfer(int index, uint64_t lba, uint32_t count, uint8_t *buffer, int writing)
{
    const struct ahci_device *d;
    int result = check_request(index, lba, count, buffer, &d);

    if (result != AHCI_OK || count == 0) return result;

    while (count > 0) {
        uint32_t chunk = count < CHUNK_SECTORS ? count : CHUNK_SECTORS;
        uint8_t fis[20];

        if (d->lba48) make_fis(fis, writing ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT);
        else          make_fis(fis, writing ? ATA_WRITE_DMA     : ATA_READ_DMA);
        set_lba(fis, lba, (uint16_t)chunk, d->lba48);

        result = issue(d->port, fis, writing, buffer, chunk * SECTOR_BYTES, RW_TIMEOUT_MS);
        if (result != AHCI_OK) return result;

        lba += chunk;
        count -= chunk;
        buffer += chunk * SECTOR_BYTES;
    }

    if (writing) {
        /* The disk may still hold the data in its own cache. Same reason
         * as the IDE driver: ask it to commit before saying we are done. */
        uint8_t fis[20];

        make_fis(fis, d->lba48 ? ATA_FLUSH_EXT : ATA_FLUSH);
        result = issue(d->port, fis, 0, 0, 0, FLUSH_TIMEOUT_MS);
    }
    return result;
}

int ahci_read(int index, uint64_t lba, uint32_t count, void *buffer)
{
    return transfer(index, lba, count, (uint8_t *)buffer, 0);
}

int ahci_write(int index, uint64_t lba, uint32_t count, const void *buffer)
{
    /* The controller only reads from the buffer on a write; the cast
     * just lets both directions share one path. */
    return transfer(index, lba, count, (uint8_t *)(uintptr_t)buffer, 1);
}

const char *ahci_error(int code)
{
    switch (code) {
    case AHCI_OK:          return "ok";
    case AHCI_ERR_NODEV:   return "no such SATA disk";
    case AHCI_ERR_RANGE:   return "past the end of the disk";
    case AHCI_ERR_BUFFER:  return "buffer cannot be used for DMA";
    case AHCI_ERR_DEVICE:  return "the disk reported an error";
    case AHCI_ERR_TIMEOUT: return "the disk did not answer in time";
    default:               return "unknown error";
    }
}
