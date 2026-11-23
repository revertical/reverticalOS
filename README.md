# ReverticalOS

A small, from-scratch operating system for x86-64: a custom BIOS two-stage
bootloader and a custom UEFI loader, an x86-64 long-mode kernel, software
compositor, window manager and a GUI modelled on `reverticalos_ui.jpeg`. No Linux,
no GRUB, no Limine, no libc.

It also has its own storage stack - ATA PIO, AHCI and NVMe drivers behind one
block layer, an MBR partition layer, the RVFS filesystem, and a GUI installer
that writes a BIOS boot chain plus a UEFI system partition back onto a disk -
plus a 64-bit module format (`.lib` / `.rvx`), a polled xHCI USB stack for
keyboards, mice and touchscreens, an e1000 network stack and a browser.

## Run it

`make` produces three bootable artifacts:

- `build/revertical.img` - a raw 32 MiB BIOS hard-disk image
- `build/revertical.iso` - a live CD with **both** a BIOS El Torito entry and a
  UEFI El Torito entry (own loaders, no GRUB)
- `build/bootx64.efi` - the UEFI loader, also embedded in the installer

The live CD is the easiest way to test the installer against a second disk:
boot from the CD, install onto the target disk, then boot that disk alone. The
installed disk can be booted by BIOS or UEFI; the installer writes both paths.

**Live CD (VirtualBox, live hard disk disconnected)**

```sh
make
tools/vm.py run-iso --fresh-target
```

`run-iso` attaches only the 64 MiB target disk on IDE 0 plus the ISO, and sets
the boot order to DVD first. See [Live CD test](#live-cd-test) below.

**Hard disk (VirtualBox, the configuration used for testing)**

```sh
mkdir -p ~/.local/share/revertical
VBoxManage convertfromraw build/revertical.img \
    ~/.local/share/revertical/manual.vdi --format VDI
VBoxManage createvm --name ReverticalOS --ostype Other_64 --register
VBoxManage modifyvm ReverticalOS --memory 256 --vram 64 \
    --graphicscontroller vboxvga --nic1 none --audio-driver none \
    --x86-pae on --x86-long-mode on \
    --boot1 disk --boot2 none --boot3 none --boot4 none --rtcuseutc on
VBoxManage storagectl ReverticalOS --name IDE --add ide --controller PIIX4
VBoxManage storageattach ReverticalOS --storagectl IDE --port 0 \
    --device 0 --type hdd --medium ~/.local/share/revertical/manual.vdi
VBoxManage startvm ReverticalOS
```

The storage path is independent of the controller type now: the block layer
picks ATA PIO, AHCI or NVMe automatically. VirtualBox's default AHCI/SATA
controller works, and so does NVMe. Attach an IDE/PIIX4 disk only if you want
to exercise the legacy PIO driver specifically.

`vboxvga` is the graphics controller that exposes the VBE modes the BIOS path
uses. The UEFI path uses GOP instead and works with `vboxsvga` or the standard
UEFI firmware.
There is also a dev harness that builds the VM for you:

```sh
tools/vm.py screenshot --image build/revertical.img --out build/shot.png --delay 9
tools/vm.py run --image build/revertical.img
tools/vm.py run --keep   # reuse the previous disk, so installs survive a reboot
tools/vm.py repair       # rebuild the VM disk from build/revertical.img
tools/vm.py stop
```

The harness keeps its disk and serial log in `~/.local/share/revertical/`, not
`/tmp`, so reboots and `/tmp` cleanups do not leave the VM pointing at a
missing file. `run` and `screenshot` re-attach a fresh disk automatically when
the old one is gone; `repair` forces that from the command line. Revert to the
pristine image at any time with `tools/vm.py repair`.

**ISO on real hardware:** write `build/revertical.iso` to a CD/DVD (or attach
it as a virtual optical drive). It boots on legacy/BIOS and on UEFI machines:
the disc carries both an El Torito BIOS boot image and a UEFI El Torito entry
for `/EFI/BOOT/BOOTX64.EFI`. Keep the installer target attached as a second
disk. GRUB is not involved anywhere.

**QEMU** (optional)

```sh
qemu-system-x86_64 -m 256 -drive format=raw,file=build/revertical.img
```

**UEFI (x86-64):** 64-bit UEFI firmware is required; 32-bit IA32 UEFI cannot
execute the x86-64 loader. The custom loader opens the ESP through Simple File
System, gets a GOP framebuffer, reads the ACPI RSDP, sets up identity page
tables and enters the kernel in long mode. Secure Boot must be off because the
loader is not signed.

**USB stick:** the ISO is hybrid - its system area carries an MBR with a
type-`0xEF` partition covering the FAT16 ESP - so a raw write is enough. Real
firmware does not use the El Torito entry on USB mass storage; it finds
`\EFI\BOOT\BOOTX64.EFI` on the ESP and adds a UEFI boot entry.

```sh
sudo dd if=build/revertical.iso of=/dev/sdX bs=4M conv=fsync status=progress
sync
```

Replace `/dev/sdX` with the **whole** stick, not a partition; this destroys its
contents. Press F12 (or the vendor's one-time boot menu) and choose the **UEFI**
entry for the stick. The same stick also boots on legacy BIOS: its MBR stub
chainloads the El Torito stage 1.

In VirtualBox, switch the firmware with:

```sh
VBoxManage modifyvm ReverticalOS --firmware efi64
VBoxManage startvm ReverticalOS
VBoxManage modifyvm ReverticalOS --firmware bios   # back to BIOS
```

Use **`efi64`, not `efi`**. VirtualBox treats `efi` as the 32-bit IA32 firmware
(`VBoxEFI-x86.fd`), which only looks for `\EFI\BOOT\BOOTIA32.EFI`; it cannot
load the x86-64 `BOOTX64.EFI` and BDS reports `Not Found` for the CD. The
x86-64 firmware is `VBoxEFI-amd64.fd`, selected with `--firmware efi64`. You
can confirm the active firmware in `VBox.log`: 32-bit boots log
`EFI: VBoxDbg> loadimage32 ...`, 64-bit boots log `loadimage64`.

**Guest CPU type:** the x86-64 firmware and kernel need PAE and long mode, so a
VM created with a 32-bit type (`--ostype Other`) must be upgraded once:

```sh
VBoxManage modifyvm ReverticalOS --ostype Other_64 --x86-pae on --x86-long-mode on
```

Without this, `efi64` dies at about one second with **Guru Meditation 1155
(`VINF_EM_TRIPLE_FAULT`)** before any `RVEFI:` output: the x86-64 firmware's
first `mov cr4` (enable PAE) raises `#GP`, and with no IDT loaded yet that
becomes `#DF` and a triple fault. `VBox.log` shows `EnablePAE = 0`, `PAE = 0`
and `LM - AMD64 Long Mode = 0`. The 32-bit IA32 firmware did not need PAE, so
the same VM booted to BDS before. This is a CPU-model setting, not an ISO or
loader problem.

The `tools/vm.py` harness drives the BIOS path; use the commands above to test
UEFI by hand. `tools/vm.py run-iso` upgrades an `efi`/`efi32` VM to `efi64` and
a 32-bit guest CPU model to `Other_64` + PAE + long mode automatically. The
loader logs `RVEFI:` milestones to COM1, so a headless UEFI boot can be followed
from the VM serial port (`--uart1 0x3F8 4 --uartmode1 file <log>` in
VBoxManage) without a graphical console.

**Real hardware:** for BIOS boot, `dd` the raw disk image
(`dd if=build/revertical.img of=/dev/sdX bs=4M conv=fsync`) and boot it in
legacy mode. For UEFI, write the hybrid ISO as described above. Either way the
kernel needs an x86-64 CPU; 32-bit-only machines are no longer supported.

The bootloader picks the best available VBE 32bpp linear framebuffer mode; 1280x1024
is preferred, and the UI scales with the display height.

## What works

- x86-64 long-mode kernel: identity page tables, own GDT/IDT, 64-bit syscall-free ABI
- Custom stage 1 + stage 2 BIOS bootloaders, kernel loaded from disk over INT 13h LBA
- Custom UEFI PE32+ loader: GOP framebuffer, Simple File System kernel load, ACPI RSDP
- El Torito live CD with BIOS **and** UEFI entries, no GRUB/Limine
- VBE (BIOS) and GOP (UEFI) framebuffers, direct linear framebuffer, no paging
- Procedural wallpaper, QOI asset loader (assets compiled into the kernel)
- Boot animation: aurora background, orbiting rings and particles, logo/wordmark
  fade-in, staged progress bar, then a cross-fade into the desktop
- Desktop shell: frosted pill with wordmark and quick actions, centred dock,
  live clock/date, battery + Wi-Fi + signal card with iridescent border
- Start menu on the pill wordmark: every app plus Lock, Shut down and Restart,
  with mouse and keyboard navigation
- Notification toasts in the top-right and a dynamic bell panel with read
  state, unread badge and Clear all
- User accounts in `/System/users` (`RVUS1`, PBKDF2-HMAC-SHA1 password hashes);
  the installer asks for them and the booted system shows a login screen
- Lock screen from the Start menu, unlocked with the current account password
- Shutdown and restart screens backed by ACPI (`RSDP` → `RSDT` → `FADT`/DSDT
  `_S5_`), with a safe-to-switch-off screen when the firmware refuses
- Window manager: drag, focus, minimise, close, open animation, hover states, tooltips
- PS/2 and USB mice with the same hardware cursor
- Five dock apps - Browser, Files, Messages, Settings, Revertical AI - plus
  the live Installer and a Program Output console
- Input: polled PS/2 keyboard/mouse, Synaptics and Elan touchpad detection,
  polled xHCI USB with HID boot and report-descriptor keyboards/mice,
  absolute USB touchscreens and precision touchpads
- Panic subsystem: exception handlers 0-31, register dump, serial and VGA
  fallback, plus a `panic()` API and a full-screen panic page
- Storage: one block layer over ATA PIO (LBA28), AHCI (SATA) and NVMe; MBR
  partition table in stage 1
- RVFS: 4 KiB blocks, 64-byte inodes, 12 direct + 1 indirect block, bitmaps
- Files app browses the mounted RVFS volume and previews text files
- Custom 64-bit module format: `.rvx` executables and `.lib` libraries with
  named imports/exports, RELA-based relocations, private stacks and a kernel API
- Intel PRO/1000 (e1000) polling driver, ARP, IPv4, ICMP, UDP, DHCP, DNS,
  TCP and an HTTP/1.0 client - all written from scratch, no sockets and no libc
- Wi-Fi station with a simulated radio: scan, WPA2-PSK association, real
  PBKDF2/HMAC-SHA1 key derivation and a four-way handshake
- Browser app with an address bar, history, HTML-to-text rendering and links
- GUI installer in the live environment: create an account, pick a disk, erase,
  write MBR, stage 2, kernel, modules, a FAT16 ESP and a fresh RVFS volume,
  verify, then restart through the power screen
- Installed systems boot on BIOS **and** UEFI; the ESP carries
  `/EFI/BOOT/BOOTX64.EFI` and `/KERNEL.BIN`

## Disk layout

A BIOS-booted install and the live hard-disk image share the same front section:

```
LBA 0            stage 1 (MBR + partition table)
LBA 1..16        stage 2
LBA 17           kernel header ("RVK1")
LBA 18..         flat kernel binary
```

An installed disk then has two partitions:

```
LBA 2048..       (live image only) RVFS volume, partition type 0x52
LBA 4096..36863  FAT16 EFI System Partition, type 0xEF (16 MiB)
                   /EFI/BOOT/BOOTX64.EFI
                   /KERNEL.BIN
LBA 36864..      RVFS data volume, partition type 0x52
```

The live hard-disk image is built by `tools/mkimage.py` with a single RVFS
partition at LBA 2048. The installer writes the two-partition layout above, so
the same disk boots on old BIOS machines through stage 1 and on UEFI machines
through the ESP. The kernel mounts the type-`0x52` partition, preferring the one
on the boot disk.

The live CD has a separate, fixed layout (2048-byte blocks):

```
LBA 0..15        ISO 9660 system area
LBA 16           Primary Volume Descriptor
LBA 17           El Torito Boot Record Volume Descriptor
LBA 18           Volume Descriptor Set Terminator
LBA 19           El Torito Boot Catalog (BIOS entry + EFI section)
LBA 20           cdstage1.bin (2048-byte BIOS boot image)
LBA 21..24       stage2-cd.bin (8 KiB, kernel block size 2048)
LBA 25           kernel header (RVK1: size and load address)
LBA 26..         flat kernel binary, padded to 2048-byte blocks
next             ISO 9660 root / EFI / BOOT directories
next             L and M path tables
next             FAT16 ESP (the UEFI El Torito image)
last             /EFI/BOOT/BOOTX64.EFI and /KERNEL.BIN file data
```

The exact LBAs move with the kernel size; `make iso` prints the final map.

The volume descriptor set is contiguous (16, 17, 18) as ISO 9660 requires; the
boot catalog follows the terminator. The ISO carries a second El Torito entry
for UEFI, pointing at the embedded ESP, and also exposes a real ISO 9660 tree
with `/EFI/BOOT/BOOTX64.EFI` and `/KERNEL.BIN`. UEFI firmware mounts the FAT16
image directly; tools that instead scan the ISO filesystem find the same files.
Either way it contains our own PE32+ loader and the kernel, so no GRUB or shim
is involved.

The CD stage 2 reports `boot_drive = 0xFFFFFFFF` to the kernel, meaning "no
attached hard disk is the running system". That is why the installer does not
tag a disk as `RUNNING` on the live CD, and why Files starts with no volume.
The ISO is padded to at least 1 MiB so BIOSes that expect a minimum disc size
are happy.

`tools/rvfs.py` and `kernel/rvfs.c` implement the same on-disk format, so a
volume can be inspected or built on the host:

```sh
python3 tools/rvfs.py list  build/revertical.img
python3 tools/rvfs.py check build/revertical.img
```

## Installing

Boot the live image, then click the **Install ReverticalOS** card in the top-left
of the desktop. The installer first asks for the account you will use to sign in:
a username (2-20 lowercase characters, starting with a letter) and a password
(4-40 printable characters, typed twice). Then pick a target disk, click *Erase
and install* twice (the second click confirms), and wait for the progress bar.
The installer:

1. writes a new MBR and partition table (type `0xEF` ESP and type `0x52` RVFS),
2. writes its built-in stage 1 and stage 2 bootloaders (the running kernel
   embeds the disk-build binaries, so this also works from the live CD),
3. writes the kernel header and the kernel image straight out of memory,
4. builds a 16 MiB FAT16 EFI System Partition containing the embedded
   `BOOTX64.EFI` loader and `/KERNEL.BIN`,
5. formats an RVFS volume and creates `/Documents`, `/Pictures`, `/System`,
   `/System/Apps`, `/System/Libs`, `/Users` and `/Users/<your-name>`,
6. writes `/System/users` with the account and its PBKDF2-HMAC-SHA1 password
   hash (the salt is `RVOS1:` followed by the lowercase user name),
7. writes the embedded demo modules (`rvstd.lib`, `hello.rvx`,
   `systeminfo.rvx`) so the installed disk can run them,
8. writes `/System/installed` last and verifies the volume by reading it back.

The installed disk is bootable in both modes: BIOS firmware runs the MBR at
LBA 0, UEFI firmware finds `\EFI\BOOT\BOOTX64.EFI` on the ESP. If Secure Boot
is enabled, turn it off or enroll the loader yourself; it is unsigned.

That marker file is what hides the desktop shortcut and enables the login
screen on the next boot. Volumes without `/System/users` (such as older installs)
skip the login and go straight to the desktop. Installing is destructive -
everything on the selected disk is replaced.

### Two-disk test

The dev harness attaches two drives automatically:

| IDE port | Disk | Purpose |
|---|---|---|
| 0 | `revertical-test.vdi` (32 MiB, from the image) | live system, boots first |
| 1 | `revertical-target.vdi` (64 MiB, blank) | installer target |

```sh
tools/vm.py run --keep
```

Open the Installer: two disks are listed, the 32 MB one tagged `RUNNING`, and
the 64 MB blank one. Select the **64 MB** disk and install onto it. Nothing on
the live disk changes, so the shortcut stays visible and the completion screen
says *Installed to VBOX HARDDISK* with a note that it is not the boot disk.

`Restart now` (shown as *Restart live system* when installing to another disk)
reboots the VM, and the BIOS still boots IDE 0 - the live disk. A guest cannot
reorder the VM's disks, so boot the disk the installer wrote from the host:

```sh
tools/vm.py stop
tools/vm.py boot-installed   # target alone on IDE 0; live disk/CD detached
```

The login screen should come up for the account created during installation
(and the Install ReverticalOS card is gone because the `/System/installed`
marker exists). Sign in, then Files should show the installed tree.
`tools/vm.py reset-target` blanks the second disk again, and
`tools/vm.py repair` rebuilds both disks from `build/revertical.img`.

### Live CD test

This is the flow that avoids the BIOS "first hard disk always boots" rule
completely: the live system is on the CD, and the hard disk is only the
install target.

```sh
make
tools/vm.py run-iso --fresh-target
```

`run-iso` attaches **only** the 64 MiB `revertical-target.vdi` on IDE 0 plus
`build/revertical.iso` on IDE 1, then boots DVD before disk. The 32 MiB live
disk is not attached at all.

1. The desktop comes up from the CD with the **Install ReverticalOS** card.
   Files shows no volume, because a CD has no RVFS volume.
2. Open the Installer. The first page asks for the account you will sign in
   with; fill in a username, password and confirmation, then continue. The only
   listed disk is the 64 MB target and nothing is tagged `RUNNING` - the CD is
   the running system, not a disk.
3. Select it, click *Erase and install* twice, and wait for *Installation
   complete*. The completion screen says *Installed. Eject the CD and boot
   this disk on its own.*
4. Stop the VM, then boot the freshly installed disk with no live disk and no
   CD attached:

```sh
tools/vm.py stop
tools/vm.py boot-installed
```

The installed system starts at the login screen for the account created in the
installer. After signing in there is no Install card, and Files shows the
installer tree (`/Documents`, `/Pictures`, `/System/installed`,
`/System/users`, `/Users/<your-name>`). This is the same disk the old two-disk
flow could never reach, because VirtualBox always booted the live disk on IDE 0
first.

To try again with a blank target:

```sh
tools/vm.py stop
tools/vm.py run-iso --fresh-target
```

`--fresh-target` blanks the 64 MiB disk; without it, the installer can reinstall
over the previous volume.

## Accounts, lock, notifications and power

`/System/users` is a small text database:

```
RVUS1
alice 3f0c9d... (64 lowercase hex characters)
```

Each digest is PBKDF2-HMAC-SHA1 over the password with the salt `RVOS1:` plus
the lowercase user name (4096 rounds, 32-byte key). The first account's
username is pre-filled on the login screen and can be replaced by typing. A
wrong password clears the field and shows an inline error.

Click the pill wordmark or logo to open the Start menu. It lists all seven apps
and three actions:

- **Lock** returns to a password-only screen for the current account (it is
  disabled when the volume has no account, for example a live CD).
- **Shut down** shows the shutdown screen, asks ACPI to put the machine into
  S5, and falls back to a *safe to switch off* page if the firmware ignores it.
- **Restart** shows the restart screen and resets through the keyboard
  controller, then the `0xCF9` reset register, then a triple fault.

Notifications arrive as top-right toasts and stay in the bell panel until you
mark them read or press *Clear all*. The kernel queues up to eight events:
boot, Ethernet/Wi-Fi link up, installer completion/failure, module runs, sign-in
and unlock. The bell shows a red dot while anything is unread.

## Modules: `.rvx` executables and `.lib` libraries

ReverticalOS does not load ELF. Programs and libraries use two small custom
containers, produced by `tools/rvlink.py` from ordinary 64-bit ELF relocatable
objects (`x86-64`, `ET_REL`, RELA relocations):

| Extension | Magic | Kind | Contents |
|---|---|---|---|
| `.rvx` | `RVEX` | executable | entry point, stack size, named imports |
| `.lib` | `RVLB` | library | named exports, no imports |

Both start with the same 64-byte header (magic, version 2, kind, image size,
entry RVA, stack size, relocation/symbol/string counts, image offset, name,
checksum). The tables follow the header:

```
header (64 bytes)
relocation table     8-byte entries: image offset, width (4 or 8 bytes)
symbol table         16-byte entries: name offset, value, patch count, patch offset
string table         NUL-terminated symbol names
patch tables         8-byte entries: image offset, type (1 = ABS, 2 = REL), width
image                16-aligned flat image, linked at address 0
```

Widths are explicit so a 64-bit pointer and a 32-bit code-model reference can
both be rebased into the arena.

The loader (`kernel/rvmod.c`) copies the image into a 1 MiB arena, applies the
absolute relocations, resolves imports by name against the kernel API and every
library in `/System/Libs`, patches the call sites, then invokes the entry point
on a private 64 KiB stack. Modules execute in ring 0 inside the kernel address
space; there is no user-mode isolation yet. Executables are reclaimed after
`_start` returns.

Kernel API symbols exported to modules (`kernel/rvapi.c`):

| Symbol | Purpose |
|---|---|
| `rv_print` | append to the Program Output buffer and the serial log |
| `rv_tick_ms` | milliseconds since boot |
| `rv_alloc` | bump allocator, 128 KiB module heap |
| `rv_rand` | deterministic LCG random number |
| `rv_net_state` | 0 = down, 1 = wired, 2 = Wi-Fi |
| `rv_net_ip` | current IPv4 address as text |
| `rv_wifi_connected` | 1 when the station is associated |
| `rv_panic` | stop the system with the full-screen panic page |

Build and inspect modules with:

```sh
make apps
python3 tools/rvlink.py inspect build/apps/hello.rvx
```

To run one, open **Files**, go to `/System/Apps`, and double-click a `.rvx`.
The **Program Output** window opens with its text output, or with the loader
error (bad magic, unresolved import, checksum mismatch, and so on). Unknown
imports are reported by name.

`rvstd.lib` exports `rvstd_banner`, `rvstd_strlen`, `rvstd_itoa`, `rvstd_fib`
and `rvstd_gcd`; `hello.rvx` imports both the kernel API and that library, so it
exercises relocation, import resolution and library lookup end to end.
`systeminfo.rvx` reports uptime, network state, IP address, Wi-Fi state and heap
allocations.

## Input: PS/2, touchpads and USB HID

The PS/2 controller is polled through the shared i8042 output buffer. Laptop
firmware often leaves the pad in a vendor-specific state, so initialisation is
deliberately conservative:

- A full device reset (`0xFF`, self-test `0xAA`, ID, then a settle delay) runs
  before anything else. "Set defaults" (`0xF6`) is *not* enough: a pad left in
  4- or 6-byte mode keeps sending that format, and every 3-byte packet is then
  misread or dropped. This is what made a real Dell Latitude E7250 touchpad
  dead while emulated PS/2 mice worked.
- Every command waits for an actual `0xFA` ACK, honours `0xFE` (resend) and
  uses timeouts, so a slow, absent or confused device cannot silently
  desynchronise the byte stream.
- The controller config enables the auxiliary and keyboard clocks and keeps the
  auxiliary IRQ off, because the driver polls the output buffer.
- **Synaptics** (four `SETRES 0x00` commands, then `GETINFO` -> `0x47`) and
  **Elan** (`F5`, three `E6`, `GETINFO` -> `3C 03 C8`) probes are for
  identification only. Each probe is followed by another reset, and a Synaptics
  pad is explicitly put back into mode 0 (relative reporting, 3-byte packets).
  No vendor mode survives into normal operation.
- The pad is then configured for 100 reports/s, 8 counts/mm, 1:1 scaling and
  data reporting enabled. Anything that does not identify is driven as a plain
  relative PS/2 mouse.

All paths feed the same cursor, button state and click handling. If no byte ever
arrives, `mouse_poll()` probes the device with `GETINFO` and retries
initialisation at most twice; an idle pad answers the probe and is left alone, so
silence by itself is not treated as a fault. The result appears as
`ps/2 pointer:` in the System Info probe.

The USB stack (`kernel/usb.c`) is a polled xHCI driver: no interrupts, root-port
devices only (hubs are not enumerated), 32- or 64-byte device contexts, command
and event rings, and one interrupt-IN endpoint per HID device. It parses the HID
report descriptor, including report IDs, usage ranges, arrays, logical min/max
and signed fields, then classifies a device as:

- **keyboard** - HID usages are translated to PS/2 set-1 scancodes and fed into
  the normal keyboard queue, so every shortcut and text field works unchanged;
- **mouse** - relative X/Y and buttons go through `mouse_usb_relative()`;
- **touchscreen / precision touchpad** - absolute X/Y are scaled from the
  reported logical range to the framebuffer and injected through
  `mouse_touch_absolute()`, with the tip switch producing the click.

HID boot-protocol keyboards and mice are switched to their boot report format
when the interface advertises it; otherwise the parsed descriptor is used.
`usb_poll()` runs from the UI loop, so hot-plugging a root-port device is picked
up at runtime.

## Storage: ATA, AHCI and NVMe

`kernel/blk.c` is the single block layer used by `disk.c`, `rvfs.c` and the
installer. `blk_init()` tries the available controllers and exposes every disk
as a 512-byte-sector device:

- `kernel/ata.c` - legacy ATA PIO, LBA28, polled, no interrupts
- `kernel/ahci.c` - AHCI/SATA with polled DMA and one command list per port
- `kernel/nvme.c` - NVMe admin and I/O queues over polled submission/completion

Devices whose PCI BARs sit at or above 4 GiB are skipped, because the kernel
identity map covers the low 4 GiB. The BIOS path maps 0-4 GiB; the UEFI loader
maps 0-64 GiB and the framebuffer.

## Panic screen and API

`kernel/panic.c` provides `panic(const char *message)`. Kernel code and modules
(via the exported `rv_panic`) can call it when an invariant fails. It prints the
message, the current CPU state and a register dump to the serial port and to VGA
text mode, and paints a full-screen panic page on the framebuffer with the
message, a fault address and a short hint. Exception handlers 0-31 in
`kernel/idt.c` and `kernel/isr.asm` route CPU faults through the same path, so a
divide error, page fault or triple-fault attempt is visible instead of hanging
silently. A module can call the exported `rv_panic` to exercise the screen on
demand; the reason string and register dump also appear on the serial port.

## Networking

`kernel/e1000.c` drives Intel 82540EM/82545EM/82543/82574-class adapters
(VirtualBox's default `82540EM` is the tested one) using the legacy descriptor
rings in polling mode. `kernel/net.c` implements the rest:

- Ethernet II, ARP request/reply and an 8-entry ARP cache
- IPv4 with header checksums and a small routing decision (local subnet vs gateway)
- ICMP echo, used by `net_ping`
- UDP with checksums, a DHCP client (DISCOVER/OFFER/REQUEST/ACK, options 1/3/6)
- DNS A-record queries with an 8-entry cache
- TCP client and a tiny server (port 80), with SYN/SYN-ACK/ACK, data, FIN
- HTTP/1.0 GET with status parsing, used by the browser

There is no socket API. The UI calls `net_http_url()` and the stack polls the
NIC from `net_poll()` while requests are in flight. `about:net` in the browser
shows the interface state, MAC, address, gateway, DNS and frame counters.

Once the wired interface has a DHCP lease, `http://neverssl.com/` works through
VirtualBox NAT. There is no TLS stack, so HTTPS is rejected with a clear error.
In the installed system the network is set up at boot; the VM harness leaves the
default NAT adapter attached.

## Wi-Fi

There is no wireless chipset in the VM, so the *radio* is simulated in
`kernel/wifi.c` while the security protocol is real. The driver exposes a
`wlan0` NetIf, a scan over five access points, association and a WPA2-PSK
supplicant:

- PMK = PBKDF2-HMAC-SHA1(passphrase, SSID, 4096 rounds, 256 bits)
- PTK = 802.11 PRF (HMAC-SHA1 over sorted MACs and nonces)
- four-way handshake with an ANonce from the AP and an SNonce from the station,
  and an HMAC-SHA1 MIC check against the AP's independently derived PTK

The station subnet is `192.168.4.0/24` (AP `192.168.4.1`, station
`192.168.4.2`). The access point serves a small in-kernel site, so the browser
can exercise ARP, TCP and HTTP over the simulated link. This is a model of
Wi-Fi, not a driver for a physical card.

Open **Settings** to scan. The networks and passphrases are:

| SSID | Security | Passphrase |
|---|---|---|
| `RevertiaNet` | WPA2-PSK | `revertical` |
| `Revertia Guest` | open | - |
| `Orbit-5G` | WPA2-PSK | `orbit2026` |
| `Lab 2.4` | WPA2-PSK | `labpass` |
| `RevertiaLab` | WPA2-PSK | `revertical` |

Pick a network, click the password field and type, then press **Connect** (or
Enter). Wrong passwords fail in the PBKDF2/PTK comparison and are reported in
the settings footer. **Open browser** jumps to `http://revertical.local/`;
`about:wifi` shows the current association.

## Browser

The old Orbit app is now the browser (the dock icon is still the planet). The
toolbar has back, forward and home buttons plus an editable address bar.

- Click the address bar (or press `Ctrl+L`), type an `http://` URL, press Enter
- `Esc` leaves the address bar; `PageUp`/`PageDown` and the arrow keys scroll
- `Home`/`End` jump to the top/bottom; number keys `1`-`8` follow a listed link
- `about:home`, `about:net` and `about:wifi` are built-in pages

HTML is converted to styled text: tags, comments, entities, headings, paragraphs
and links are handled, scripts and styles are skipped, and anchors become a
clickable numbered link list at the bottom of the page. There is no CSS layout,
JavaScript or TLS.

## Revertical AI

The Revertical AI app is intentionally a designed shell: window, welcome state,
suggestion chips and composer are in place, but no model calls or system prompt are
wired up yet. That comes in the next build.

## Build

```sh
make          # rebuild assets + kernel + image + ISO + ESP
make image    # BIOS hard-disk image only
make iso      # BIOS + UEFI live CD / USB image only
make check-iso # validate El Torito, hybrid MBR, FAT image and ISO tree
make esp      # stand-alone FAT16 ESP image
make apps     # rebuild .rvx/.lib modules (also done by make)
make assets   # regenerate visual assets (needs Python 3 + Pillow)
```

Requires `nasm`, an x86-64-capable `gcc`, `ld`, `objcopy`, `clang` (for the
UEFI PE32+ loader) and Python 3 with Pillow. On Debian/Ubuntu:

```sh
sudo apt install build-essential binutils nasm clang make python3 python3-pil
```

The kernel is freestanding `-m64`/`-mcmodel=small`; no libc, no libgcc. The
freestanding 64-bit division helpers live in `kernel/div64.c`.

## Layout

```
boot/stage1.asm    MBR + partition table, loads disk stage 2
boot/stage2.asm    A20, VBE probe, identity page tables, long-mode entry
                   (-DCDBOOT builds the 2048-byte-sector CD variant)
boot/cdstage1.asm  El Torito no-emulation boot image, loads stage 2 from ISO
boot/mbr.asm       hybrid MBR stub, chainloads the CD stage 1 from USB
boot/uefi/loader.c custom UEFI PE32+ loader (GOP, Simple File System, ACPI)
kernel/            kernel, compositor, desktop shell, boot animation
kernel/start.asm   long-mode entry, BSS clear, stack, calls kmain
kernel/panic.c     panic page, register dump, serial/VGA fallback
kernel/idt.c       IDT and exception handlers
kernel/blk.c       block layer over ATA/AHCI/NVMe
kernel/ata.c       ATA PIO driver (identify/read/write/flush)
kernel/ahci.c      AHCI/SATA polled DMA driver
kernel/nvme.c      NVMe queue-pair driver
kernel/disk.c      MBR parsing, partition and raw-device helpers
kernel/rvfs.c      RVFS filesystem
kernel/accounts.c  /System/users accounts and PBKDF2 password hashes
kernel/power.c     ACPI RSDP/RSDT/FADT lookup, S5 shutdown, reset
kernel/installer.c installer state machine, ESP writer
kernel/rvmod.c     RVLB/RVEX module loader, symbols and relocations
kernel/rvapi.c     kernel API exported to modules
kernel/usb.c       polled xHCI, HID descriptors, keyboard/mouse/touch
kernel/mouse.c     PS/2 mouse, Synaptics/Elan detect, pointer injection
kernel/e1000.c     Intel PRO/1000 polling driver
kernel/net.c       ARP/IPv4/ICMP/UDP/DHCP/DNS/TCP/HTTP stack
kernel/wifi.c      simulated radio, scan, WPA2-PSK supplicant
kernel/sha1.c      SHA-1, HMAC-SHA-1, PBKDF2
kernel/kbd.c       polled PS/2 keyboard
apps/*.c           demo modules (rvstd library, hello, systeminfo)
tools/rvlink.py    ELF64 -> RVLB/RVEX linker and inspector
tools/genblob.py   embeds the boot chain, loader and modules in the kernel
tools/mkesp.py     FAT16 EFI System Partition builder
tools/mkiso.py     ISO 9660 + dual El Torito + hybrid MBR writer
tools/             asset generator, image builder, RVFS host tool, VM helper
reverticalos_ui.jpeg  desktop UI design reference (asset build input)
.github/workflows/build.yml  CI: build the images and run make check-iso
```

## License

MIT - see `LICENSE`. The Poppins fonts are under the SIL Open Font License 1.1
(`tools/fonts/OFL.txt`); see `THIRD_PARTY.md` for the full list.

## Contributing

See `CONTRIBUTING.md`. In short: build with `make`, run `make check-iso`, keep
both the BIOS and UEFI paths working, and keep the boot stack from scratch.
