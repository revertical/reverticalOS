#include "types.h"
#include "bootinfo.h"
#include "serial.h"
#include "io.h"
#include "string.h"
#include "gfx.h"
#include "assets.h"
#include "timer.h"
#include "rtc.h"
#include "mouse.h"
#include "kbd.h"
#include "net.h"
#include "rvmod.h"
#include "bootanim.h"
#include "blk.h"
#include "disk.h"
#include "rvfs.h"
#include "installer.h"
#include "accounts.h"
#include "power.h"
#include "ui.h"
#include "idt.h"
#include "panic.h"
#include "usb.h"

extern u8 __bss_start[];
extern u8 __bss_end[];

static void fade_to_desktop(const u32 *from)
{
    const int steps = 26;
    for (int i = 0; i <= steps; i++) {
        int lin = i * 255 / steps;
        int t = lin * lin * (765 - 2 * lin) / 65025;
        gfx_present_fade(from, (u8)t);
        if (i < steps)
            timer_delay_ms(26);
    }
}

void kmain(BootInfo *bi)
{
    memset(__bss_start, 0, (size_t)(__bss_end - __bss_start));

    serial_init();
    serial_write("\r\nReverticalOS kernel alive\r\n");
    idt_init();

    if (!bi || bi->magic != BOOTINFO_MAGIC)
        panic("bootloader passed invalid BootInfo");

    serial_write("Framebuffer: ");
    serial_dec(bi->fb_phys);
    serial_write(" mode ");
    serial_hex(bi->mode);
    serial_write(" ");
    serial_dec(bi->width);
    serial_write("x");
    serial_dec(bi->height);
    serial_write(" pitch ");
    serial_dec(bi->pitch);
    serial_write(" bpp ");
    serial_dec(bi->bpp);
    serial_write("\r\n");

    gfx_init(bi);
    assets_init();
    gfx_load_backgrounds();
    gfx_fonts_init();

    timer_init();
    mouse_init((int)gfx.width / 2, (int)gfx.height / 2,
               (int)gfx.width - 1, (int)gfx.height - 1);
    kbd_init();
    power_init(bi->acpi_rsdp);

    usb_init();

    blk_init();
    disk_init();

    const DiskPart *volume = 0;
    if (bi->boot_drive == 0xFFFFFFFEu) {
        /* UEFI has no BIOS drive number: mount the first RVFS volume. */
        for (int i = 0; i < disk_part_count(); i++) {
            if (disk_part(i)->type == 0x52) {
                volume = disk_part(i);
                break;
            }
        }
    } else if (bi->boot_drive != 0xFFFFFFFFu) {
        int boot_slot = (int)(bi->boot_drive & 0x7F);
        for (int i = 0; i < disk_part_count(); i++) {
            if (disk_part(i)->type == 0x52 &&
                disk_part(i)->slot == boot_slot) {
                volume = disk_part(i);
                break;
            }
        }
    }
    if (volume && rvfs_mount(volume) == RVFS_OK) {
        const RvfsVolume *v = rvfs_volume();
        serial_write("rvfs: mounted '");
        serial_write(v->label);
        serial_write("' ");
        serial_dec((v->total_blocks - v->free_blocks) * v->block_size / 1024);
        serial_write(" KiB used of ");
        serial_dec(v->total_blocks * v->block_size / 1024);
        serial_write(" KiB\r\n");
    } else {
        serial_write("rvfs: no volume mounted\r\n");
    }

    installer_init(bi->boot_drive);
    accounts_init();

    rvmod_init();
    net_init();
    serial_write("net: ");
    serial_write(net_status_line());
    serial_write("\r\n");

    RtcTime now;
    rtc_read(&now);
    serial_write("rtc: ");
    serial_dec((u32)now.year);
    serial_write("-");
    serial_dec((u32)now.month);
    serial_write("-");
    serial_dec((u32)now.day);
    serial_write(" ");
    serial_dec((u32)now.hour);
    serial_write(":");
    serial_dec((u32)now.minute);
    serial_write("\r\n");

    boot_anim_run();

    /* The final boot frame lives in gfx.wallpaper. Stash it, restore the
       wallpaper, then cross-fade into the freshly composed desktop. */
    memcpy(gfx.bootbg, gfx.wallpaper,
           (size_t)gfx.width * (size_t)gfx.height * 4);
    gfx_reload_wallpaper();

    ui_init();
    ui_render();
    fade_to_desktop(gfx.bootbg);
    gfx_present();

    serial_write("desktop: online\r\n");
    ui_run();
}
