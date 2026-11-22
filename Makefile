CC      = gcc
LD      = ld
LINKER  = seaweed/linker.ld

# Shared by both halves. -MMD -MP writes a .d file per object so a change
# to a header rebuilds whatever includes it.
CFLAGS  = -m32 -march=i686 -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mno-3dnow -std=gnu99 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -MMD -MP

# Seaweed sees its own internal headers. Yunix only sees the public door
# (seaweed.h, string.h) plus its own headers - so a Yunix file that tries
# to include vga.h or io.h fails to compile instead of quietly reaching
# into the kernel.
SEAWEED_CFLAGS = $(CFLAGS) -Iseaweed/include -Iseaweed/include/internal \
                 -DINSTALL_KERNEL_LBA=$(INSTALL_KERNEL_LBA) \
                 -DINSTALL_KERNEL_SECTORS=$(INSTALL_KERNEL_SECTORS)
YUNIX_CFLAGS   = $(CFLAGS) -Iseaweed/include -Iyunix/include

# Where "install" puts the kernel on a disk: raw sectors in the gap
# between the partition table and the first partition at 2048, right
# after GRUB's core image. The installed GRUB loads exactly these
# sectors, and the installer writes exactly here - both come from these
# two numbers, so they cannot drift apart.
INSTALL_KERNEL_LBA     = 1024
INSTALL_KERNEL_SECTORS = 1022

# GRUB for an installed disk: its 512-byte boot sector, and a core image
# with just what booting Yunix needs - BIOS disk access, the MBR
# partition reader and the multiboot loader - and a two-line script.
GRUB_DIR      = /usr/lib/grub/i386-pc
GRUB_BOOT_IMG = build/grub-boot.img
GRUB_CORE_IMG = build/grub-core.img
GRUB_EMBED    = build/grub-embed.cfg

LDFLAGS = -m elf_i386 -T $(LINKER) -nostdlib

KERNEL  = seaweed.bin
ISO     = yunix.iso
DISK    = disk.img

ASRC    = $(wildcard seaweed/boot/*.s)
KSRC    = $(wildcard seaweed/kernel/*.c seaweed/drivers/*.c seaweed/lib/*.c seaweed/fs/*.c)
YSRC    = $(wildcard yunix/*.c yunix/apps/*.c)
OBJS    = $(patsubst %.s,build/%.o,$(ASRC)) \
          $(patsubst %.c,build/%.o,$(KSRC)) \
          $(patsubst %.c,build/%.o,$(YSRC))

# Programs: static 32-bit ELF files that run in ring 3 and talk to the
# kernel with Linux's system calls - so each one also runs on Linux.
# No C library yet, hence -nostdlib and _start instead of main. -no-pie
# gives a plain executable at a fixed address, the only kind the loader
# takes so far.
PROGRAM_CFLAGS = -m32 -march=i686 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
                 -fno-pie -no-pie -static -nostdlib
PROGRAM_SRC    = $(wildcard programs/*.c)
PROGRAMS       = $(patsubst programs/%.c,build/programs/%.elf,$(PROGRAM_SRC))

all: $(KERNEL) $(PROGRAMS)

build/programs/%.elf: programs/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PROGRAM_CFLAGS) -o $@ $<

programs: $(PROGRAMS)

build/seaweed/%.o: seaweed/%.s
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

build/seaweed/%.o: seaweed/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SEAWEED_CFLAGS) -c $< -o $@

build/yunix/%.o: yunix/%.c
	@mkdir -p $(dir $@)
	$(CC) $(YUNIX_CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) $(LINKER)
	$(LD) $(LDFLAGS) -o $(KERNEL) $(OBJS)
	@echo "Built $(KERNEL)"

-include $(OBJS:.o=.d)

$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1M count=8 2>/dev/null
	@echo "Created $(DISK)"

disk: $(DISK)

$(GRUB_EMBED): Makefile
	@mkdir -p build
	@# The root device is the disk the BIOS booted from, so a blocklist with
	@# no device name means "these sectors of this disk".
	printf 'multiboot $(INSTALL_KERNEL_LBA)+$(INSTALL_KERNEL_SECTORS)\nboot\n' > $@

$(GRUB_CORE_IMG): $(GRUB_EMBED)
	grub-mkimage -O i386-pc -o $@ -c $(GRUB_EMBED) -p /boot/grub \
	    biosdisk part_msdos multiboot boot
	@# It must end before the kernel's sectors start.
	@test $$(( ($$(stat -c %s $@) + 511) / 512 )) -lt $(INSTALL_KERNEL_LBA) || \
	    (echo "GRUB core image too big for the gap before the kernel"; false)

$(GRUB_BOOT_IMG): $(GRUB_DIR)/boot.img
	@mkdir -p build
	cp $< $@

grub: $(GRUB_CORE_IMG) $(GRUB_BOOT_IMG)

# What the boot loader hands the kernel: the kernel itself and GRUB's two
# pieces (for "install"), and every program. The live ISO's grub.cfg loads
# the same files.
MODULES = $(KERNEL) seaweed.bin,$(GRUB_BOOT_IMG) boot.img,$(GRUB_CORE_IMG) core.img$(foreach p,$(PROGRAMS),,$(p) $(basename $(notdir $(p))))

run: $(KERNEL) $(DISK) grub $(PROGRAMS)
	qemu-system-i386 -kernel $(KERNEL) -initrd "$(MODULES)" \
	    -drive file=$(DISK),format=raw,if=ide -display curses

run-gui: $(KERNEL) grub $(PROGRAMS)
	qemu-system-i386 -kernel $(KERNEL) -initrd "$(MODULES)"

$(ISO): $(KERNEL) grub.cfg grub $(PROGRAMS)
	rm -rf iso
	mkdir -p iso/boot/grub iso/boot/programs
	cp $(KERNEL) iso/boot/$(KERNEL)
	cp $(GRUB_BOOT_IMG) iso/boot/grub-boot.img
	cp $(GRUB_CORE_IMG) iso/boot/grub-core.img
	cp $(PROGRAMS) iso/boot/programs/
	@# grub.cfg with one "module" line per program in place of the line
	@# that says @PROGRAMS@.
	{ sed '/@PROGRAMS@/,$$d' grub.cfg; \
	  for p in $(PROGRAMS); do \
	      n=$$(basename $$p .elf); echo "    module /boot/programs/$$n.elf $$n"; \
	  done; \
	  sed '1,/@PROGRAMS@/d' grub.cfg; } > iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) iso 2>/dev/null
	@echo "Built $(ISO)"

iso: $(ISO)

clean:
	rm -rf build $(KERNEL) $(ISO) iso

.PHONY: all run run-gui iso grub programs clean
