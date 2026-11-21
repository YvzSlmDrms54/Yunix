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
SEAWEED_CFLAGS = $(CFLAGS) -Iseaweed/include -Iseaweed/include/internal
YUNIX_CFLAGS   = $(CFLAGS) -Iseaweed/include -Iyunix/include

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

all: $(KERNEL)

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

run: $(KERNEL) $(DISK)
	qemu-system-i386 -kernel $(KERNEL) -drive file=$(DISK),format=raw,if=ide -display curses

run-gui: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL)

$(ISO): $(KERNEL) grub.cfg
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/$(KERNEL)
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) iso 2>/dev/null
	@echo "Built $(ISO)"

iso: $(ISO)

clean:
	rm -rf build $(KERNEL) $(ISO) iso

.PHONY: all run run-gui iso clean
