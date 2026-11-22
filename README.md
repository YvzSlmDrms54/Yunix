# Yunix

_A hobby operating system for x86, built on the Seaweed kernel._

Early development: multiboot header, kernel entry, VGA text-mode driver.

## Minimum system requirements

| | Minimum | Recommended |
|---|---|---|
| **CPU** | 32-bit x86, i686 class (Pentium Pro or later) | Any x86 or x86-64 CPU |
| **RAM** | 4 MB | 16 MB or more |
| **Firmware** | Legacy BIOS, or UEFI with CSM / Legacy boot enabled | |
| **Display** | VGA-compatible, 80×25 text mode | |
| **Keyboard** | PS/2, or USB with "USB legacy support" in the BIOS | |
| **Boot media** | CD or USB stick (`yunix.iso`, booted by GRUB) | |
| **Disk** (optional) | IDE/ATA disk on the primary master, or SATA set to IDE/Legacy mode in the BIOS | |
| **Disk space** | About 260 KB for a Fish partition | 1 MB or more |

Without a disk, Yunix still runs, but files only live in RAM and are lost on reboot.

> [!WARNING]
> Yunix has only been tested in QEMU and VirtualBox so far. Pure UEFI machines
> (without CSM) show a black screen, and AHCI (SATA) and NVMe disks are not
> supported yet.

### Virtual machines
- **VirtualBox:** type *Other/Unknown*, 64 MB RAM. Attach the disk to an **IDE**
  controller (not SATA): disk on IDE Primary Master, ISO on IDE Secondary Master.

## Building

### Make the ISO file

### **1. Install WSL**

To install WSL, run this in either PowerShell or CMD:

```bash
wsl --install
```

Once WSL's finished installing, run `wsl` and run this (might take a few minutes):

```bash
sudo apt install build-essential gcc-multilib qemu-system-x86 grub-pc-bin xorriso
```

This will automaticcally download and install WSL to your system and download the requirements to make the ISO file.


### **2. Make the ISO**

Once they finish installing, clone this repository at your home folder (~) if you haven't, cd into it and run:

```bash
make iso
```

This will make the ISO for you to put into VirtualBox or VMWare.

### **3. Run in QEMU**

In WSL, run

```bash
make run
```

to boot Yunix in QEMU or

```bash
make run-gui
```

to boot in an external window.

**You can,**
- run `halt` to stop Yunix.
- press ALT+F2 and press `q` and enter to exit QEMU.
