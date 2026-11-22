# Yunix

_A hobby operating system for x86, built on the Seaweed kernel._

Yunix is written from scratch in C and assembly - no Linux, no libc underneath.
It boots from a live CD or USB stick, keeps your files on disk in its own
filesystem (Fish), and can install itself onto a disk so it starts without the
CD.

**What's in it so far**

- **Seaweed, the kernel:** GDT and IDT, interrupts, paging (with NULL pointer
  protection), a heap, PCI bus scanning, and a crash screen that shows where
  things went wrong
- **Disks:** IDE and SATA (AHCI) disks, MBR and GPT partition tables
- **Fish, the filesystem:** directories and files up to 4 KB each, saved inside
  its own partition - it never writes anywhere else on a disk
- **The Yunix shell:** 30+ commands, line editing with the arrow keys, command
  history, `&&` / `||` / `;` chaining
- **yvim:** a small vi-style text editor
- **Programs in ring 3:** ordinary C programs built against the musl C library
  run as separate, protected programs. Seaweed answers the 32-bit Linux system
  calls, so the same binary runs on Linux and on Yunix - and a crashing
  program is stopped while Yunix keeps going
- **An installer** that puts Yunix on a disk with GRUB

## Minimum system requirements

| | Minimum | Recommended |
|---|---|---|
| **CPU** | 32-bit x86, i686 class (Pentium Pro or later) | Any x86 or x86-64 CPU |
| **RAM** | 8 MB | 16 MB or more |
| **Firmware** | Legacy BIOS, or UEFI with CSM / Legacy boot enabled | |
| **Display** | VGA-compatible, 80×25 text mode | |
| **Keyboard** | PS/2, or USB with "USB legacy support" in the BIOS | |
| **Boot media** | CD or USB stick (`yunix.iso`, booted by GRUB) | |
| **Disk** (optional) | IDE disk on the primary master, or a SATA disk (AHCI) | |
| **Disk space** | About 260 KB for a Fish partition; 3 MB to install Yunix | 16 MB or more |

Without a disk, Yunix still runs, but files only live in RAM and are lost on reboot.

> [!WARNING]
> Yunix has only been tested in QEMU and VirtualBox so far. Pure UEFI machines
> (without CSM) show a black screen, and NVMe disks are not supported yet.

### Virtual machines
- **VirtualBox:** type *Other/Unknown*, 64 MB RAM. The disk can go on the **SATA**
  (AHCI) controller - the default - or on **IDE Primary Master**. Put the ISO on
  the IDE controller.
- **QEMU:** `make run` (below) sets everything up. For a SATA machine, add
  `-machine q35`.

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

This will automatically download and install WSL to your system and download the requirements to make the ISO file.


### **2. Make the ISO**

Once they finish installing, clone this repository at your home folder (~) if you haven't, cd into it and run:

```bash
make iso
```

This will make the ISO for you to put into VirtualBox or VMWare. Besides the
kernel, it carries the two pieces of GRUB that `install` writes to a disk, and
the programs from `programs/`.

The first build also downloads [musl](https://musl.libc.org), the C library
programs are built with (about 1 MB, checked against its published SHA-256),
and compiles it into `build/musl`. That takes a few minutes, once: `make clean`
keeps it, and only `make distclean` removes it.

### **3. Run in QEMU**

In WSL, run

```bash
make run
```

to boot Yunix in QEMU or

```bash
make run-gui
```

to boot in an external window. Both attach `disk.img` (8 MB, made the first time)
as the disk, and pass the installer its files, so `install` works there too.

**You can,**
- run `halt` to stop Yunix.
- press ALT+F2 and press `q` and enter to exit QEMU.

## Using Yunix

Type `help` for every command (`help 2` for the second page). A few to start with:

| Command | What it does |
|---|---|
| `ls`, `cd`, `mkdir`, `touch`, `cat`, `rm` | Work with files and folders |
| `yvim <file>` | Edit a file (`i` to type, `Esc`, then `:wq` to save and quit) |
| `save` | Write your files to the disk - **nothing is saved until you do** |
| `disk` | List the disks and show which one Fish uses (`disk <n>` to move it) |
| `format yes` | Make a Fish partition on a blank disk and start empty |
| `partition` | Show partitions and free space; `partition new <n>` makes a Fish partition there |
| `install` | Install Yunix on a disk (see below) |
| `run` / `run <name>` | List the programs, or run one (try `run hello`) |
| `pci`, `uptime`, `neofetch`, `user`, `hostname` | Look around and personalise |

↑ and ↓ go through earlier commands, ← and → move inside the line, and
commands can be chained: `mkdir notes && cd notes`.

## Installing Yunix

1. Boot `yunix.iso` from a CD or USB stick (or in a virtual machine).
2. Try it out. Files you make and `save` along the way come with you.
3. Run `install` to see which disks Yunix can go on, then `install <number>` to
   see exactly what will be written, and `install <number> yes` to do it.
4. Take out the CD or USB stick and reboot. Yunix now starts from the disk.

What it writes: GRUB's boot code in the first sector, GRUB itself and the kernel
in the first megabyte, and a Fish partition after that.

> [!IMPORTANT]
> Writing boot code replaces whatever boot loader a disk had, so for now the
> installer **only uses a disk that is blank, or holds nothing but a Fish
> partition**. Disks with Windows, Linux or any other data on them - and GPT
> disks - are refused, and nothing on them is changed. Installing next to
> another system is planned. Until it exists, use an empty disk or a spare
> computer.

Yunix can still *keep files* on a disk that has other systems on it, without
installing: free up some space with Windows' **Disk Management** ("Shrink
Volume") or **GParted**, then use `partition new` to make a Fish partition in
that free space. Yunix never shrinks or moves another partition itself.
