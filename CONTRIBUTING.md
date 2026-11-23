# Contributing to ReverticalOS

Thanks for taking a look. ReverticalOS is a from-scratch x86-64 operating
system: its own BIOS stage 1/stage 2, its own UEFI PE32+ loader, its own
freestanding kernel and drivers. There is no Linux, GRUB, Limine or libc in
the boot path, and contributions should keep it that way.

## Build

Debian/Ubuntu:

```sh
sudo apt install build-essential binutils nasm clang make python3 python3-pil
make
make check-iso
```

`make` builds the BIOS hard-disk image, the hybrid live ISO and the UEFI
loader. `python3-pil` (Pillow) is only needed by the asset generator.

Other distributions need the same tools: `nasm`, an x86-64-capable `gcc`,
GNU `ld` with PE support (`i386pep`), `objcopy`, `clang`, `make` and Python 3
with Pillow.

## Testing

Static image checks:

```sh
make check-iso
```

Quick QEMU smoke test (BIOS):

```sh
qemu-system-x86_64 -m 256 -drive format=raw,file=build/revertical.img
```

QEMU + x86-64 OVMF (UEFI):

```sh
qemu-system-x86_64 -m 512 -bios /usr/share/ovmf/OVMF.fd \
  -drive file=build/revertical.iso,media=cdrom,readonly=on \
  -boot d -serial file:serial.log -display none -no-reboot
```

VirtualBox harness:

```sh
tools/vm.py run-iso      # live CD + target disk
tools/vm.py boot-installed
```

The harness upgrades an old `efi`/`efi32` VM to `efi64`, and a 32-bit guest
CPU model to `Other_64` with PAE and long mode, because the x86-64 firmware
and kernel require both.

Real hardware: the ISO is hybrid. `dd` it to a USB stick, disable Secure Boot
(the loader is unsigned) and pick the UEFI entry from the one-time boot menu.
32-bit-only machines are not supported.

## Code style

- C11, freestanding: no libc, libgcc, SSE or red zone. Keep the kernel inside
  the linker script's limits (the flat binary must stay below 1.5 MiB).
- 4-space indentation for C and assembly. Tabs in the `Makefile`.
- Prefer small, hardware-focused functions and comments that explain the
  register or protocol behaviour, not just the syntax.
- Python tools should stay close to the standard library; Pillow is the only
  third-party runtime dependency.
- Both the BIOS and UEFI boot paths must keep working. If you touch the image
  layout, update `tools/mkiso.py`, `tools/checkiso.py` and the README together.

## Pull requests

- Keep a change focused and explain what hardware, firmware or VM you tested.
- Run `make` and `make check-iso` before opening a PR.
- Update the README when behaviour or the build changes.
- Do not add third-party boot code, and do not copy code with an incompatible
  license.

By submitting a contribution you agree that it may be distributed under the
project's MIT license (see `LICENSE`).

Be straightforward and respectful in issues and reviews.
