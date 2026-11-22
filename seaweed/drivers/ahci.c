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

/* Sends a command that reads one 512-byte block into 'buffer', using
 * command slot 0, and waits for it. Returns 0 on success. */
static int issue_read_command(int port, uint8_t command, void *buffer)
{
    volatile uint32_t *p = port_regs(port);
    struct port_memory *m = &memory[port];
    uint32_t *header = (uint32_t *)m->command_list;
    uint8_t  *fis = m->command_table;
    uint32_t *prd = (uint32_t *)(m->command_table + 0x80);

    if (wait_for(&p[PORT_TFD], TFD_BSY | TFD_DRQ, 0, 1000) != 0) return -1;

    /* Slot 0's header: the command FIS is 5 dwords long, data flows from
     * the device (no write bit), and there is one PRD entry. */
    header[0] = 5u | (1u << 16);
    header[1] = 0;                              /* bytes moved, filled in by the HBA */
    header[2] = (uint32_t)m->command_table;
    header[3] = 0;

    memset(m->command_table, 0, sizeof(m->command_table));
    fis[0] = FIS_REG_H2D;
    fis[1] = 0x80;                              /* this FIS carries a command */
    fis[2] = command;
    fis[7] = 0;                                 /* device register */

    /* One region: 512 bytes into 'buffer'. The count field holds the
     * length minus one. */
    prd[0] = (uint32_t)buffer;
    prd[1] = 0;
    prd[2] = 0;
    prd[3] = 512 - 1;

    p[PORT_IS] = 0xFFFFFFFFu;
    p[PORT_CI] = 1;

    {
        uint32_t deadline = timer_ticks() + TIMER_HZ * 2;      /* two seconds */

        while (p[PORT_CI] & 1) {
            if (p[PORT_IS] & IS_TFES) return -1;
            if ((int32_t)(timer_ticks() - deadline) > 0) return -1;
        }
    }

    if (p[PORT_TFD] & TFD_ERR) return -1;
    return 0;
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
    if (issue_read_command(port, command, identify_buffer) != 0) return;

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
