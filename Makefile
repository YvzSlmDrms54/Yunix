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

# --- programs ---
#
# Static 32-bit ELF files that run in ring 3 and talk to the kernel with
# Linux's system calls - so each one also runs on Linux. -no-pie gives a
# plain executable at a fixed address, the only kind the loader takes so
# far. No SSE: Seaweed does not switch it on, so those instructions would
# fault.
#
#   programs/*.c       ordinary C with main(), built against musl
#   programs/raw/*.c   no C library at all: _start and int $0x80 by hand
PROGRAM_ARCH   = -m32 -march=i686 -mno-sse -mno-sse2 -mno-mmx
PROGRAM_CFLAGS = $(PROGRAM_ARCH) -O2 -Wall -Wextra -fno-pie -no-pie -static

RAW_SRC        = $(wildcard programs/raw/*.c)
MUSL_SRC       = $(wildcard programs/*.c)
PROGRAMS       = $(patsubst programs/raw/%.c,build/programs/%.elf,$(RAW_SRC)) \
                 $(patsubst programs/%.c,build/programs/%.elf,$(MUSL_SRC))

# musl, the C library: built from source into build/musl, so nothing has
# to be installed system-wide. The download is checked against musl's
# published SHA-256 before anything is built from it.
MUSL_VERSION   = 1.2.5
MUSL_SHA256    = a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
MUSL_TAR       = build/musl-$(MUSL_VERSION).tar.gz
MUSL_PREFIX    = $(CURDIR)/build/musl
MUSL_SPECS     = build/musl/lib/musl-gcc.specs

all: $(KERNEL) $(PROGRAMS)

$(MUSL_TAR):
	@mkdir -p build
	curl -fsSL -o $@.part https://musl.libc.org/releases/musl-$(MUSL_VERSION).tar.gz
	echo "$(MUSL_SHA256)  $@.part" | sha256sum -c --quiet -
	mv $@.part $@

$(MUSL_SPECS): $(MUSL_TAR)
	rm -rf build/musl-src && mkdir -p build/musl-src
	tar -xzf $(MUSL_TAR) -C build/musl-src --strip-components=1
	@# --target=i386 picks the architecture; the host's gcc does the work
	@# with -m32, and the host's ar and ranlib (not "i386-ar") pack it up.
	cd build/musl-src && ./configure --prefix=$(MUSL_PREFIX) --target=i386 \
	    --disable-shared --enable-wrapper=gcc \
	    CC="gcc $(PROGRAM_ARCH)" > /dev/null
	$(MAKE) -C build/musl-src -j4 AR=ar RANLIB=ranlib > /dev/null
	$(MAKE) -C build/musl-src install AR=ar RANLIB=ranlib > /dev/null
	@echo "Built musl $(MUSL_VERSION) into build/musl"

musl: $(MUSL_SPECS)

build/programs/%.elf: programs/raw/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PROGRAM_CFLAGS) -ffreestanding -fno-stack-protector -nostdlib -o $@ $<

build/programs/%.elf: programs/%.c $(MUSL_SPECS)
	@mkdir -p $(dir $@)
	@# musl's specs file replaces gcc's link rule, dropping the "32-bit" it
	@# would pass to the linker - so say it again.
	$(CC) $(PROGRAM_CFLAGS) -specs $(MUSL_SPECS) -Wl,-m,elf_i386 -o $@ $<

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

# Keeps the built musl and its download: rebuilding them takes minutes
# and needs the internet. "make distclean" removes those too.
clean:
	if [ -d build ]; then find build -mindepth 1 -maxdepth 1 ! -name musl \
	    ! -name 'musl-*.tar.gz' -exec rm -rf {} +; fi
	rm -rf $(KERNEL) $(ISO) iso

distclean:
	rm -rf build $(KERNEL) $(ISO) iso

.PHONY: all run run-gui iso grub programs musl clean distclean
