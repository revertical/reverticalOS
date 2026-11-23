#!/usr/bin/env python3
"""VirtualBox harness for running ReverticalOS during development.

Usage:
    tools/vm.py run --keep          # live system on the 32 MiB disk
    tools/vm.py run-iso             # live CD, no live disk attached
    tools/vm.py boot-installed      # boot the disk written by the installer
    tools/vm.py reset-target        # blank the 64 MiB target disk
    tools/vm.py repair              # rebuild both disks from the image
    tools/vm.py stop

Disk mode attaches the live system on IDE port 0 and a blank 64 MiB disk on
port 1. `run-iso` instead attaches only the target disk plus the El Torito
ISO, so the live hard disk is genuinely disconnected: boot from CD, install
onto the second disk, then use `boot-installed` to boot that disk alone.

The disks and the serial log live under ~/.local/share/revertical, so a reboot
that clears /tmp does not leave the VM pointing at a missing file.
"""
import argparse
import shutil
import subprocess
import time
from pathlib import Path

VM_NAME = "ReverticalOS Dev"
STATE_DIR = Path.home() / ".local/share/revertical"
LIVE_DISK = STATE_DIR / "revertical-test.vdi"
TARGET_DISK = STATE_DIR / "revertical-target.vdi"
SERIAL_LOG = STATE_DIR / "revertical-serial.log"
LIVE_UUID = "5a3c9d61-7f42-4c8e-9b57-000000000001"
TARGET_MB = 64

# Older builds kept this in /tmp; clean up the registry entry if present.
LEGACY_VDI = Path("/tmp/opencode/revertical-test.vdi")


def run(cmd, check=True, quiet=True):
    p = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if check and p.returncode != 0:
        print("command failed:", " ".join(cmd))
        print(p.stdout)
        raise SystemExit(1)
    if not quiet:
        print(p.stdout.strip())
    return p


def vbm(*args, check=True, quiet=True):
    return run(["VBoxManage", *args], check=check, quiet=quiet)


def vm_exists():
    p = vbm("list", "vms", check=False)
    return f'"{VM_NAME}"' in p.stdout


def is_running():
    p = vbm("list", "runningvms", check=False)
    return f'"{VM_NAME}"' in p.stdout


def vm_readable():
    return vbm("showvminfo", VM_NAME, check=False).returncode == 0


def has_ide_controller():
    p = vbm("showvminfo", VM_NAME, check=False)
    return p.returncode == 0 and "'IDE'" in p.stdout


def vm_info():
    """Per-VM settings from `showvminfo --machinereadable`, as a dict."""
    p = vbm("showvminfo", VM_NAME, "--machinereadable", check=False)
    info = {}
    for line in p.stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            info[key] = value.strip('"')
    return info


def firmware():
    """Return the VM's firmware mode in lower case ('bios', 'efi', 'efi32', ...)."""
    return (vm_info().get("firmware") or "").lower() or None


def ensure_64bit_cpu():
    """The kernel is x86-64, so the VM's modeled CPU needs PAE and long mode.

    A VM created with a 32-bit guest type (for example `--ostype Other`)
    models a 32-bit CPU with PAE and long mode masked. The x86-64 EFI
    firmware then triple-faults on its very first `mov cr4` (enabling PAE),
    before it can print anything: VirtualBox shows a Guru Meditation and no
    `RVEFI:` lines appear, which looks like a broken ISO but is the CPU model.
    """
    info = vm_info()
    if info.get("pae") == "on" and info.get("longmode") == "on":
        return
    print("guest CPU is 32-bit (PAE/long mode off); enabling a 64-bit CPU model")
    if "64-bit" not in info.get("ostype", ""):
        vbm("modifyvm", VM_NAME, "--ostype", "Other_64", check=False)
    vbm("modifyvm", VM_NAME, "--x86-pae", "on", "--x86-long-mode", "on",
        check=False)


def ensure_x64_efi():
    """The UEFI loader is x86-64; IA32 firmware only looks for BOOTIA32.EFI."""
    mode = firmware()
    if mode in ("efi", "efi32"):
        print("firmware is 32-bit IA32 EFI; switching to 64-bit EFI (efi64)")
        vbm("modifyvm", VM_NAME, "--firmware", "efi64", check=False)


def stop_vm():
    if is_running():
        vbm("controlvm", VM_NAME, "poweroff", check=False)
        time.sleep(1.0)


def create_vm():
    print(f"creating VM '{VM_NAME}'")
    # A 64-bit guest type matters: `Other` is the 32-bit profile and masks
    # PAE/long mode, which the x86-64 EFI firmware and kernel require.
    vbm("createvm", "--name", VM_NAME, "--ostype", "Other_64", "--register")
    vbm("modifyvm", VM_NAME,
        "--memory", "256", "--vram", "64",
        "--nic1", "none", "--audio-driver", "none",
        "--boot1", "disk", "--boot2", "none", "--boot3", "none", "--boot4", "none",
        "--graphicscontroller", "vboxvga",
        "--x86-pae", "on", "--x86-long-mode", "on",
        "--rtcuseutc", "on")


def ensure_vm():
    stop_vm()
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    if vm_exists() and not vm_readable():
        # A truncated config or a lost media registry can leave the VM
        # unstartable. It is only a test VM, so recreate it.
        print(f"VM '{VM_NAME}' is unreadable; recreating it")
        vbm("unregistervm", VM_NAME, "--delete", check=False)
    if not vm_exists():
        create_vm()
    ensure_64bit_cpu()
    # Settings that must stay current (the serial log moved out of /tmp).
    vbm("modifyvm", VM_NAME, "--uart1", "0x3F8", "4",
        "--uartmode1", "file", str(SERIAL_LOG), check=False)
    if not has_ide_controller():
        vbm("storagectl", VM_NAME, "--name", "IDE", "--add", "ide",
            "--controller", "PIIX4")
    vbm("storagectl", VM_NAME, "--name", "Floppy", "--add", "floppy",
        "--remove", check=False)


def set_boot_order(dvd_first):
    """Boot DVD before disk (live CD) or disk before DVD (installed system)."""
    order = ("dvd", "disk") if dvd_first else ("disk", "dvd")
    vbm("modifyvm", VM_NAME,
        "--boot1", order[0], "--boot2", order[1],
        "--boot3", "none", "--boot4", "none", check=False)


def detach_media():
    """Empty every IDE slot and forget stale disk media registry entries."""
    for port in (0, 1):
        for device in (0, 1):
            for kind in ("hdd", "dvddrive"):
                vbm("storageattach", VM_NAME, "--storagectl", "IDE",
                    "--port", str(port), "--device", str(device),
                    "--type", kind, "--medium", "none", check=False)
    for target in (str(LIVE_DISK), str(TARGET_DISK), LIVE_UUID, str(LEGACY_VDI)):
        vbm("closemedium", "disk", target, check=False)


def disk_usable(path):
    return path.exists() and path.stat().st_size >= 1024 * 1024


def create_live_disk(image):
    image = Path(image)
    if not image.exists():
        raise SystemExit(f"image not found: {image} (run make first)")
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    if LIVE_DISK.exists():
        LIVE_DISK.unlink()
    print(f"creating live disk {LIVE_DISK} from {image}")
    run(["VBoxManage", "convertfromraw", str(image), str(LIVE_DISK),
         "--format", "VDI"])
    # Stable identity so the VM media registry keeps working across rebuilds.
    vbm("internalcommands", "sethduuid", str(LIVE_DISK), LIVE_UUID)


def create_target_disk():
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    if TARGET_DISK.exists():
        TARGET_DISK.unlink()
    print(f"creating blank {TARGET_MB} MiB target disk {TARGET_DISK}")
    # createmedium registers the medium already, so leave its UUID alone:
    # changing it afterwards would leave an inaccessible registry entry.
    vbm("createmedium", "disk", "--filename", str(TARGET_DISK),
        "--size", str(TARGET_MB), "--format", "VDI")


def attach_disk(path, port=0, device=0):
    vbm("storageattach", VM_NAME, "--storagectl", "IDE",
        "--port", str(port), "--device", str(device), "--type", "hdd",
        "--medium", str(path))


def attach_dvd(iso):
    vbm("storageattach", VM_NAME, "--storagectl", "IDE",
        "--port", "1", "--device", "0", "--type", "dvddrive",
        "--medium", str(iso))


def reset_target(image):
    ensure_vm()
    detach_media()
    create_target_disk()
    if not disk_usable(LIVE_DISK):
        create_live_disk(image)
    attach_disk(LIVE_DISK, 0)
    attach_disk(TARGET_DISK, 1)
    set_boot_order(False)
    print(f"target disk reset: {TARGET_DISK} is blank")


def attach_media(boot="live"):
    disks = [(TARGET_DISK, 0), (LIVE_DISK, 1)]
    if boot != "target":
        disks = [(LIVE_DISK, 0), (TARGET_DISK, 1)]
    for path, port in disks:
        if path.exists():
            attach_disk(path, port)


def attach_image(image, keep=False, boot="live"):
    ensure_vm()
    detach_media()
    set_boot_order(False)

    if boot == "target":
        if not disk_usable(TARGET_DISK):
            raise SystemExit("no installed target disk; install to the second "
                             "disk first (run, then use the Installer)")
        # Booting the target is pointless if the live disk gets rebuilt.
        keep = True

    if keep and disk_usable(LIVE_DISK):
        print(f"reusing live disk {LIVE_DISK}")
    else:
        if keep:
            print(f"live disk missing or unusable, rebuilding from {image}")
        create_live_disk(image)

    # The target keeps whatever the installer wrote until an explicit reset.
    if not disk_usable(TARGET_DISK):
        create_target_disk()

    if boot == "target":
        print("boot: installed target disk (IDE 0)")
    else:
        print("boot: live disk (IDE 0)")
    attach_media(boot)


def run_iso(iso, vm_type="gui", fresh_target=False):
    iso = Path(iso)
    if not iso.exists():
        raise SystemExit(f"ISO not found: {iso} (run make iso first)")
    ensure_vm()
    ensure_x64_efi()
    detach_media()
    if fresh_target or not disk_usable(TARGET_DISK):
        create_target_disk()

    # The live hard disk is deliberately not attached at all.
    attach_disk(TARGET_DISK, 0)
    attach_dvd(iso)
    set_boot_order(True)
    print(f"boot: live CD {iso}")
    print(f"disk: {TARGET_DISK} on IDE 0 (installer target)")
    start(vm_type)


def boot_installed(vm_type="gui"):
    ensure_vm()
    detach_media()
    if not disk_usable(TARGET_DISK):
        raise SystemExit("no installed target disk; install onto the 64 MiB "
                         "target first (run-iso, then use the Installer)")
    attach_disk(TARGET_DISK, 0)
    set_boot_order(False)
    print(f"boot: installed disk {TARGET_DISK} (IDE 0, no live disk, no CD)")
    start(vm_type)


def start(vm_type="headless"):
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    if SERIAL_LOG.exists():
        SERIAL_LOG.unlink()
    p = vbm("startvm", VM_NAME, "--type", vm_type, quiet=False)
    if p.returncode != 0:
        print("VirtualBox could not start the VM.")
        print("Try: tools/vm.py repair")


def stop():
    stop_vm()


def screenshot(image, out, delay):
    attach_image(image)
    start()
    print(f"waiting {delay:.1f}s ...")
    time.sleep(delay)
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE_DIR / "vbox-shot.png"
    vbm("controlvm", VM_NAME, "screenshotpng", str(tmp), quiet=False)
    shutil.copyfile(tmp, out)
    stop()
    print(f"screenshot: {out}")
    if SERIAL_LOG.exists():
        print("--- serial ---")
        print(SERIAL_LOG.read_text(errors="replace"))


def repair(image):
    ensure_vm()
    detach_media()
    create_live_disk(image)
    create_target_disk()
    attach_disk(LIVE_DISK, 0)
    attach_disk(TARGET_DISK, 1)
    set_boot_order(False)
    print(f"VM '{VM_NAME}' repaired: live disk from {image}, target blank")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("screenshot")
    sp.add_argument("--image", default="build/revertical.img")
    sp.add_argument("--out", default="build/screenshot.png")
    sp.add_argument("--delay", type=float, default=6.0)

    rp = sub.add_parser("run")
    rp.add_argument("--image", default="build/revertical.img")
    rp.add_argument("--type", default="gui")
    rp.add_argument("--keep", action="store_true",
                    help="reuse the live disk so installs and file writes persist")
    rp.add_argument("--boot", choices=("live", "target"), default="live",
                    help="boot the live system or the system installed on the "
                         "second disk")

    ip = sub.add_parser("run-iso",
                        help="boot the live CD with only the target disk attached")
    ip.add_argument("--image", default="build/revertical.iso")
    ip.add_argument("--type", default="gui")
    ip.add_argument("--fresh-target", action="store_true",
                    help="blank the 64 MiB target disk before starting")

    bp = sub.add_parser("boot-installed",
                        help="boot the disk written by the installer")
    bp.add_argument("--type", default="gui")

    pp = sub.add_parser("repair")
    pp.add_argument("--image", default="build/revertical.img")

    tp = sub.add_parser("reset-target")
    tp.add_argument("--image", default="build/revertical.img")
    sub.add_parser("stop")

    args = ap.parse_args()

    if args.cmd == "screenshot":
        screenshot(args.image, args.out, args.delay)
    elif args.cmd == "run":
        attach_image(args.image, keep=args.keep, boot=args.boot)
        start(args.type)
    elif args.cmd == "run-iso":
        run_iso(args.image, args.type, fresh_target=args.fresh_target)
    elif args.cmd == "boot-installed":
        boot_installed(args.type)
    elif args.cmd == "repair":
        repair(args.image)
    elif args.cmd == "reset-target":
        reset_target(args.image)
    elif args.cmd == "stop":
        stop()


if __name__ == "__main__":
    main()
