# ReverticalOS build system
#
# Requires: nasm, an x86-64-capable gcc, ld (elf_x86_64), objcopy, clang for
# the UEFI loader, and python3.
# The kernel is freestanding: no libc, no Linux, no GRUB/Limine.

CC       := gcc
NASM     := nasm
LD       := ld
OBJCOPY  := objcopy
PYTHON   := python3
CLANG    ?= clang

BUILD    := build
CFLAGS   := -m64 -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
            -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-builtin \
            -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone \
            -mcmodel=small -mno-sse -mno-sse2 -mno-mmx -mno-80387 -msoft-float \
            -Ikernel -I$(BUILD)
LDFLAGS  := -m elf_x86_64 -nostdlib --build-id=none -T kernel/linker.ld

KERNEL_C := $(wildcard kernel/*.c)
KERNEL_ASM := $(wildcard kernel/*.asm)
ASSETS_BIN := $(BUILD)/assets.bin
ASSETS_OBJ := $(BUILD)/assets_blob.o
KERNEL_OBJ := $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(KERNEL_C)) \
              $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(KERNEL_ASM)) \
              $(ASSETS_OBJ) $(BUILD)/bootblob.o

.PHONY: all clean image iso check-iso esp assets kernel apps run run-iso run-vbox screenshot

all: image iso

# Visual assets are generated from tools/genassets.py (Python + Pillow).
$(ASSETS_BIN): tools/genassets.py reverticalos_ui.jpeg $(wildcard tools/fonts/*.ttf)
	$(PYTHON) tools/genassets.py

$(ASSETS_OBJ): $(ASSETS_BIN)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 $(ASSETS_BIN) $@

assets: $(ASSETS_BIN)

$(BUILD)/stage1.bin: boot/stage1.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin $< -o $@

$(BUILD)/stage2.bin: boot/stage2.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin $< -o $@

# ISO (El Torito) boot chain: a 2048-byte no-emulation stage 1 and the same
# stage 2 assembled for 2048-byte CD blocks (kernel at LBA 25/26).
$(BUILD)/cdstage1.bin: boot/cdstage1.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin $< -o $@

$(BUILD)/stage2-cd.bin: boot/stage2.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin -DCDBOOT $< -o $@

# Hybrid MBR stub: lets a USB stick written from the ISO boot on legacy BIOS.
$(BUILD)/mbr.bin: boot/mbr.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin $< -o $@

# UEFI loader: a freestanding x86-64 PE/EFI application.  clang targets the
# Windows GNU ABI and ld emits a PE32+ image with base relocations.
UEFI_CFLAGS := -ffreestanding -fshort-wchar -mno-red-zone -fno-stack-protector \
               -fno-builtin -O2 -Wall -Wextra

$(BUILD)/bootx64.efi: boot/uefi/loader.c
	@mkdir -p $(dir $@)
	$(CLANG) --target=x86_64-unknown-windows-gnu $(UEFI_CFLAGS) -c $< \
		-o $(BUILD)/bootx64.o
	$(LD) -m i386pep --subsystem 10 -e efi_main \
		--image-base 0x10000000 -o $@ $(BUILD)/bootx64.o

# FAT16 EFI System Partition: /EFI/BOOT/BOOTX64.EFI plus /KERNEL.BIN.
$(BUILD)/esp.img: $(BUILD)/bootx64.efi $(BUILD)/kernel.bin tools/mkesp.py
	$(PYTHON) tools/mkesp.py --loader $(BUILD)/bootx64.efi \
		--kernel $(BUILD)/kernel.bin --output $@

esp: $(BUILD)/esp.img

# Modules built into RVEX/RVLB containers by tools/rvlink.py.
RVSTD_EXPORTS := rvstd_banner rvstd_strlen rvstd_itoa rvstd_fib rvstd_gcd

$(BUILD)/apps/%.o: apps/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/apps/rvstd.lib: $(BUILD)/apps/rvstd.o tools/rvlink.py
	$(PYTHON) tools/rvlink.py lib --name rvstd --object $< \
		$(foreach s,$(RVSTD_EXPORTS),--export $(s)) --output $@

$(BUILD)/apps/hello.rvx: $(BUILD)/apps/hello.o $(BUILD)/apps/rvstd.lib tools/rvlink.py
	$(PYTHON) tools/rvlink.py exe --name hello --object $< --output $@

$(BUILD)/apps/systeminfo.rvx: $(BUILD)/apps/systeminfo.o tools/rvlink.py
	$(PYTHON) tools/rvlink.py exe --name systeminfo --object $< --output $@

apps: $(BUILD)/apps/rvstd.lib $(BUILD)/apps/hello.rvx $(BUILD)/apps/systeminfo.rvx

# Disk boot binaries and module files embedded in the kernel so the installer
# also works when the live system booted from a CD with no boot disk.
$(BUILD)/bootblob.h: tools/genblob.py $(BUILD)/stage1.bin \
		$(BUILD)/stage2.bin $(BUILD)/bootx64.efi $(BUILD)/apps/rvstd.lib \
		$(BUILD)/apps/hello.rvx $(BUILD)/apps/systeminfo.rvx
	$(PYTHON) tools/genblob.py --stage1 $(BUILD)/stage1.bin \
		--stage2 $(BUILD)/stage2.bin --loader $(BUILD)/bootx64.efi \
		--file rvstd.lib=$(BUILD)/apps/rvstd.lib \
		--file hello.rvx=$(BUILD)/apps/hello.rvx \
		--file systeminfo.rvx=$(BUILD)/apps/systeminfo.rvx \
		--output $(BUILD)/bootblob.c --header $(BUILD)/bootblob.h

$(BUILD)/bootblob.c: $(BUILD)/bootblob.h

$(BUILD)/bootblob.o: $(BUILD)/bootblob.c $(BUILD)/bootblob.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# installer.c includes the generated embedded-file header.
$(BUILD)/kernel/installer.o: $(BUILD)/bootblob.h

$(BUILD)/kernel/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/kernel.elf: $(KERNEL_OBJ) kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJ)
	@size $@ | tail -n 1

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	@bss=$$(nm $< | awk '/__bss_end/{print $$1}'); \
	if [ -n "$$bss" ] && [ $$((0x$$bss)) -gt $$((0x2800000)) ]; then \
		echo "kernel BSS ends at 0x$$bss, above the UEFI loader reservation (0x2800000)"; \
		exit 1; \
	fi
	$(OBJCOPY) -O binary $< $@
	@echo "kernel.bin: $$(stat -c%s $@) bytes"

kernel: $(BUILD)/kernel.bin

image: $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin
	$(PYTHON) tools/mkimage.py --stage1 $(BUILD)/stage1.bin \
		--stage2 $(BUILD)/stage2.bin --kernel $(BUILD)/kernel.bin \
		--apps-dir $(BUILD)/apps --output $(BUILD)/revertical.img

iso: $(BUILD)/cdstage1.bin $(BUILD)/stage2-cd.bin $(BUILD)/kernel.bin \
		$(BUILD)/esp.img $(BUILD)/bootx64.efi $(BUILD)/mbr.bin
	$(PYTHON) tools/mkiso.py --cdstage1 $(BUILD)/cdstage1.bin \
		--stage2 $(BUILD)/stage2-cd.bin --kernel $(BUILD)/kernel.bin \
		--esp $(BUILD)/esp.img --loader $(BUILD)/bootx64.efi \
		--mbr $(BUILD)/mbr.bin --output $(BUILD)/revertical.iso

check-iso: iso
	$(PYTHON) tools/checkiso.py $(BUILD)/revertical.iso

run: image
	@if command -v qemu-system-i386 >/dev/null 2>&1; then \
		qemu-system-i386 -m 256 -drive file=$(BUILD)/revertical.img,format=raw,if=ide \
			-vga std -machine pc; \
	else \
		$(PYTHON) tools/vm.py run --image $(BUILD)/revertical.img; \
	fi

run-iso: iso
	$(PYTHON) tools/vm.py run-iso --image $(BUILD)/revertical.iso

run-vbox: image
	$(PYTHON) tools/vm.py run --image $(BUILD)/revertical.img

screenshot: image
	$(PYTHON) tools/vm.py screenshot --image $(BUILD)/revertical.img --out $(BUILD)/screenshot.png

clean:
	rm -rf $(BUILD)
