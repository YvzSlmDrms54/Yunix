/* seaweed/drivers/pci.c - finding what is plugged into the PCI bus.
 *
 * Nearly every device in a PC - disk controllers, network cards, USB,
 * graphics - sits on PCI. Each one is addressed by three numbers:
 *
 *     bus (0-255) : device, also called slot (0-31) . function (0-7)
 *
 * written like 00:1f.2. A device with several functions is really
 * several devices in one chip; function 0 says whether the others exist.
 *
 * Every function has 256 bytes of "configuration space" describing it:
 *
 *     0x00  vendor ID (16 bits)      0x02  device ID (16 bits)
 *     0x08  revision, then programming interface, subclass, class
 *     0x0E  header type (bit 7 set = more functions follow)
 *     0x10  Base Address Registers (BARs): where the device's own
 *           registers appear, in memory or in I/O ports
 *
 * The classic way to read it is two I/O ports: write a 32-bit address to
 * 0xCF8 saying which bus, slot, function and register, then read the
 * value from 0xCFC. Every PC since the mid-1990s supports this.
 *
 * The scan only reads. The one write, pci_enable(), is made by a driver
 * for the device it is about to use.
 */

#include "pci.h"
#include "io.h"

#define PCI_ADDRESS   0xCF8
#define PCI_DATA      0xCFC

#define VENDOR_NONE   0xFFFF     /* what an empty slot reads as */

#define HEADER_GENERAL 0x00      /* an ordinary device: six BARs */
#define HEADER_BRIDGE  0x01      /* a PCI-to-PCI bridge: two BARs */

static struct pci_device devices[PCI_MAX_DEVICES];
static int               device_count;
static int               found;    /* may be more than device_count */

uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    /* Bit 31 enables the access; then bus, slot, function and register,
     * with the register's low two bits always zero. */
    uint32_t address = 0x80000000u |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)(slot & 0x1F) << 11) |
                       ((uint32_t)(func & 0x07) << 8) |
                       (offset & 0xFC);

    outl(PCI_ADDRESS, address);
    return inl(PCI_DATA);
}

void pci_enable(const struct pci_device *d)
{
    /* The command register is the low half of offset 0x04; the high half
     * is the status register, whose bits are cleared by writing 1. Writing
     * zeros there leaves the status alone. */
    uint32_t command = pci_config_read(d->bus, d->slot, d->func, 0x04) & 0xFFFF;
    uint32_t address = 0x80000000u |
                       ((uint32_t)d->bus << 16) |
                       ((uint32_t)(d->slot & 0x1F) << 11) |
                       ((uint32_t)(d->func & 0x07) << 8) | 0x04;

    command |= 0x0002;          /* memory space: answer at its BARs */
    command |= 0x0004;          /* bus master: may read and write memory (DMA) */

    outl(PCI_ADDRESS, address);
    outl(PCI_DATA, command);
}

static void remember(uint8_t bus, uint8_t slot, uint8_t func)
{
    struct pci_device *d;
    uint32_t id    = pci_config_read(bus, slot, func, 0x00);
    uint32_t class = pci_config_read(bus, slot, func, 0x08);
    uint32_t misc  = pci_config_read(bus, slot, func, 0x0C);
    int bars;

    found++;
    if (device_count >= PCI_MAX_DEVICES) return;

    d = &devices[device_count++];
    d->bus         = bus;
    d->slot        = slot;
    d->func        = func;
    d->vendor      = (uint16_t)id;
    d->device      = (uint16_t)(id >> 16);
    d->revision    = (uint8_t)class;
    d->prog_if     = (uint8_t)(class >> 8);
    d->subclass    = (uint8_t)(class >> 16);
    d->class_code  = (uint8_t)(class >> 24);
    d->header_type = (uint8_t)((misc >> 16) & 0x7F);

    bars = d->header_type == HEADER_GENERAL ? 6 :
           d->header_type == HEADER_BRIDGE  ? 2 : 0;
    for (int i = 0; i < 6; i++) {
        d->bar[i] = i < bars ? pci_config_read(bus, slot, func, (uint8_t)(0x10 + i * 4)) : 0;
    }
}

void pci_init(void)
{
    device_count = 0;
    found = 0;

    /* The simple way: ask every possible address. That is 256 x 32
     * function-0 reads, which takes well under a second. Following the
     * bridges instead would be faster, but this cannot miss anything. */
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_config_read((uint8_t)bus, slot, 0, 0x00);
            uint8_t header;

            if ((id & 0xFFFF) == VENDOR_NONE) continue;

            remember((uint8_t)bus, slot, 0);

            /* Bit 7 of the header type: the other seven functions may
             * be in use too. Without it, they must not be probed - some
             * old chips answer for function 0 at every function. */
            header = (uint8_t)(pci_config_read((uint8_t)bus, slot, 0, 0x0C) >> 16);
            if (!(header & 0x80)) continue;

            for (uint8_t func = 1; func < 8; func++) {
                id = pci_config_read((uint8_t)bus, slot, func, 0x00);
                if ((id & 0xFFFF) != VENDOR_NONE) remember((uint8_t)bus, slot, func);
            }
        }
    }
}

int pci_count(void)
{
    return device_count;
}

int pci_found(void)
{
    return found;
}

const struct pci_device *pci_get(int index)
{
    if (index < 0 || index >= device_count) return 0;
    return &devices[index];
}

const struct pci_device *pci_find(uint8_t class_code, uint8_t subclass, uint8_t prog_if)
{
    for (int i = 0; i < device_count; i++) {
        if (devices[i].class_code == class_code &&
            devices[i].subclass == subclass &&
            devices[i].prog_if == prog_if) return &devices[i];
    }
    return 0;
}

/* --- names, for people --- */

const char *pci_class_name(const struct pci_device *d)
{
    switch (d->class_code) {
    case 0x01:
        switch (d->subclass) {
        case 0x01: return "IDE controller";
        case 0x05: return "ATA controller";
        case 0x06: return d->prog_if == 0x01 ? "SATA controller (AHCI)" : "SATA controller";
        case 0x08: return d->prog_if == 0x02 ? "NVMe controller" : "Memory controller";
        default:   return "Storage controller";
        }
    case 0x02:
        return d->subclass == 0x00 ? "Ethernet controller" :
               d->subclass == 0x80 ? "Network controller" : "Network controller";
    case 0x03:
        return d->subclass == 0x00 ? "VGA display" : "Display controller";
    case 0x04:
        return d->subclass == 0x03 ? "Audio device" : "Multimedia device";
    case 0x05:
        return "Memory controller";
    case 0x06:
        switch (d->subclass) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        default:   return "Bridge";
        }
    case 0x07:
        return "Communication controller";
    case 0x08:
        return "System peripheral";
    case 0x0C:
        switch (d->subclass) {
        case 0x03:
            switch (d->prog_if) {
            case 0x00: return "USB controller (UHCI)";
            case 0x10: return "USB controller (OHCI)";
            case 0x20: return "USB controller (EHCI)";
            case 0x30: return "USB controller (xHCI)";
            default:   return "USB controller";
            }
        case 0x05: return "SMBus controller";
        default:   return "Serial bus controller";
        }
    case 0x0D:
        return "Wireless controller";
    default:
        return "Other device";
    }
}

const char *pci_vendor_name(uint16_t vendor)
{
    switch (vendor) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "AMD/ATI";
    case 0x10EC: return "Realtek";
    case 0x14E4: return "Broadcom";
    case 0x144D: return "Samsung";
    case 0x1B4B: return "Marvell";
    case 0x1234: return "QEMU";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x1B36: return "Red Hat (QEMU)";
    case 0x80EE: return "VirtualBox";
    case 0x15AD: return "VMware";
    default:     return "";
    }
}
