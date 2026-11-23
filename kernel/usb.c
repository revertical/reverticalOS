/* xHCI USB host controller driver.
 *
 * Polled (no interrupt vector) so it works on both boot paths without
 * touching the IDT.  Supports root-port devices: HID boot keyboards and
 * mice, generic HID keyboards, relative HID mice and absolute HID
 * touchscreens / precision touchpads.
 *
 * All rings and device contexts live in the kernel BSS.  The kernel maps
 * physical memory identity, so the physical address of a structure is just
 * its pointer.  Only BARs below 4 GiB are used.
 */
#include "usb.h"
#include "pci.h"
#include "io.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "mouse.h"
#include "kbd.h"
#include "gfx.h"

#define USB_MAX_DEVS 6

#define XHCI_CLASS 0x0C
#define XHCI_SUB   0x03
#define XHCI_PROGIF 0x30

/* Capability registers */
#define XCAP_CAPLENGTH  0x00
#define XCAP_HCSPARAMS1 0x04
#define XCAP_HCCPARAMS1 0x10
#define XCAP_DBOFF      0x14
#define XCAP_RTSOFF     0x18

/* Operational registers */
#define XOP_USBCMD   0x00
#define XOP_USBSTS   0x04
#define XOP_PAGESIZE 0x08
#define XOP_CRCR     0x18
#define XOP_DCBAAP   0x30
#define XOP_CONFIG   0x38

#define USBCMD_RS     0x00000001u
#define USBCMD_HCRST  0x00000002u
#define USBSTS_HCH    0x00000001u
#define USBSTS_EINT   0x00000008u
#define USBSTS_CNR    0x00000800u

/* Port status/control bits */
#define PORTSC_CCS    0x00000001u
#define PORTSC_PED    0x00000002u
#define PORTSC_PR     0x00000010u
#define PORTSC_PLS    0x000001E0u
#define PORTSC_PP     0x00000200u
#define PORTSC_CSC    0x00020000u
#define PORTSC_PRC    0x00200000u
#define PORTSC_PLC    0x00400000u
#define PORTSC_CEC    0x00800000u
#define PORTSC_WRC    0x00100000u
#define PORTSC_RW1C   0x00FE0000u
#define PORTSC_CHANGE 0x00FE0000u

/* Interrupter 0 registers (relative to RTSOFF) */
#define XIR0_IMAN   0x20
#define XIR0_ERSTSZ 0x28
#define XIR0_ERSTBA 0x30
#define XIR0_ERDP   0x38

/* TRB types */
#define TRB_NORMAL   1
#define TRB_SETUP    2
#define TRB_DATA     3
#define TRB_STATUS   4
#define TRB_LINK     6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_EP 12
#define TRB_RESET_DEVICE 15
#define TRB_TRANSFER_EVENT 32
#define TRB_CMD_COMPLETE 33
#define TRB_PORT_STATUS 34

#define TRB_CYCLE 0x00000001u
#define TRB_TC    0x00000002u
#define TRB_IOC   0x00000020u
#define TRB_DIR_IN 0x00010000u
#define TRB_TYPE(t) ((u32)(t) << 10)

#define RING_TRBS 64
#define RING_USABLE (RING_TRBS - 1)
#define EVENT_TRBS 128

typedef struct {
    u32 d0, d1, d2, d3;
} Trb;

typedef struct {
    u16 page;
    u16 usage;
    u16 umin;
    u16 umax;
    u32 off;
    u8 size;
    u8 rid;
    u8 sign;
    u8 array;
    i32 lmin;
    i32 lmax;
} HidField;

enum {
    USB_KIND_NONE = 0,
    USB_KIND_KEYBOARD,
    USB_KIND_MOUSE,
    USB_KIND_TOUCH
};

typedef struct {
    int used;
    int slot;
    int port;
    int speed;
    int iface;
    int kind;
    int dci;
    int mps;
    int xfer_len;
    int armed;
    int dead;
    int has_rid;
    int rid;
    int report_bits;
    int report_len;
    int nfields;
    HidField fields[32];
    int xf, yf, tf, bf, cf;
    u8 prev_keys[8];
    u8 prev_mod;
    u32 ep0_idx, ep0_cyc;
    u32 in_idx, in_cyc;
} UsbDev;

/* Controller state */
static volatile u8 *xhci_mmio;
static volatile u8 *xhci_op;
static volatile u8 *xhci_rt;
static volatile u8 *xhci_db;
static int xhci_ok;
static int xhci_csz;           /* 0: 32-byte contexts, 1: 64-byte */
static int max_ports;
static int max_slots;
static int need_scan;

static u32 cmd_idx, cmd_cycle;
static u32 ev_idx, ev_cycle;

static u8 cmd_ring[RING_TRBS * 16] __attribute__((aligned(64)));
static u8 event_ring[EVENT_TRBS * 16] __attribute__((aligned(64)));
static u64 dcbaa[64] __attribute__((aligned(64)));
static u8 erst[16] __attribute__((aligned(64)));

/* Per-device memory.  Rows are 64-byte aligned because the row size is a
   multiple of 64 and the base is 64-byte aligned. */
static u8 dev_ctx_mem[USB_MAX_DEVS][2048] __attribute__((aligned(64)));
static u8 in_ctx_mem[USB_MAX_DEVS][2048] __attribute__((aligned(64)));
static u8 ep0_ring_mem[USB_MAX_DEVS][RING_TRBS * 16] __attribute__((aligned(64)));
static u8 in_ring_mem[USB_MAX_DEVS][RING_TRBS * 16] __attribute__((aligned(64)));
static u8 report_buf[USB_MAX_DEVS][256] __attribute__((aligned(64)));
static u8 hid_desc[1024] __attribute__((aligned(16)));
static u8 cfg_desc[1024] __attribute__((aligned(16)));

static UsbDev devs[USB_MAX_DEVS];
static int dev_count;

/* MMIO helpers */
static inline u32 rr32(u32 off)
{
    return *(volatile u32 *)(xhci_mmio + off);
}

static inline void ww32(u32 off, u32 v)
{
    *(volatile u32 *)(xhci_mmio + off) = v;
    __asm__ volatile ("" ::: "memory");
}

static inline u64 rr64(u32 off)
{
    return *(volatile u64 *)(xhci_mmio + off);
}

static inline void ww64(u32 off, u64 v)
{
    *(volatile u64 *)(xhci_mmio + off) = v;
    __asm__ volatile ("" ::: "memory");
}

static inline void wmb(void)
{
    __asm__ volatile ("" ::: "memory");
}

static inline u64 phys(const void *p)
{
    return (u64)(uintptr_t)p;
}

/* ------------------------------------------------------------------ */
/* Rings and events                                                    */

static void trb_set(volatile Trb *t, u32 d0, u32 d1, u32 d2, u32 d3)
{
    t->d0 = d0;
    t->d1 = d1;
    t->d2 = d2;
    t->d3 = d3;
}

/* Push a TRB, inserting a link TRB and toggling the producer cycle at the
   end of the ring.  Returns the physical address of the written TRB. */
static u64 ring_push(u8 *ring, u32 *idx, u32 *cycle,
                     u32 d0, u32 d1, u32 d2, u32 d3)
{
    if (*idx >= RING_USABLE) {
        volatile Trb *link = (volatile Trb *)(ring + RING_USABLE * 16);
        trb_set(link, (u32)phys(ring), (u32)(phys(ring) >> 32), 0,
                TRB_TYPE(TRB_LINK) | TRB_TC | *cycle);
        *idx = 0;
        *cycle ^= 1;
    }
    volatile Trb *t = (volatile Trb *)(ring + *idx * 16);
    trb_set(t, d0, d1, d2, d3 | *cycle);
    u64 addr = phys((const void *)t);
    (*idx)++;
    wmb();
    return addr;
}

/* Put a link TRB at the last slot so the controller can always wrap. */
static void ring_reset(u8 *ring)
{
    memset(ring, 0, RING_TRBS * 16);
    volatile Trb *link = (volatile Trb *)(ring + RING_USABLE * 16);
    u64 a = phys(ring);
    trb_set(link, (u32)a, (u32)(a >> 32), 0,
            TRB_TYPE(TRB_LINK) | TRB_TC | 1u);
}

static int event_poll(Trb *out)
{
    volatile Trb *t = (volatile Trb *)(event_ring + ev_idx * 16);
    u32 d3 = t->d3;
    if ((d3 & 1u) != ev_cycle)
        return 0;

    out->d0 = t->d0;
    out->d1 = t->d1;
    out->d2 = t->d2;
    out->d3 = d3;

    ev_idx++;
    if (ev_idx >= EVENT_TRBS) {
        ev_idx = 0;
        ev_cycle ^= 1;
    }
    /* EHB acknowledges the event; keep the pointer at the next slot. */
    *(volatile u64 *)(xhci_rt + XIR0_ERDP) =
        (u64)phys(event_ring + ev_idx * 16) | 0x08u;
    return 1;
}

static void dispatch_event(const Trb *ev);

/* Execute a command TRB.  Returns the xHCI completion code (1 = success),
   or -1 on timeout.  Command ring doorbell is 0. */
static int xhci_cmd(u32 d0, u32 d1, u32 d2, u32 d3, u32 *slot_out)
{
    u64 trb = ring_push(cmd_ring, &cmd_idx, &cmd_cycle, d0, d1, d2, d3);
    *(volatile u32 *)(xhci_db) = 0;
    __asm__ volatile ("" ::: "memory");

    Trb ev;
    u64 start = timer_ms();
    while (timer_ms() - start < 1500) {
        if (!event_poll(&ev)) {
            __asm__ volatile ("pause");
            continue;
        }
        int type = (int)((ev.d3 >> 10) & 0x3F);
        if (type == TRB_CMD_COMPLETE) {
            u64 addr = (u64)ev.d0 | ((u64)ev.d1 << 32);
            if (addr == trb) {
                if (slot_out)
                    *slot_out = ev.d3 >> 24;
                return (int)((ev.d2 >> 24) & 0xFF);
            }
        } else {
            dispatch_event(&ev);
        }
    }
    return -1;
}

static void doorbell_ep(int slot, int dci)
{
    *(volatile u32 *)(xhci_db + slot * 4) =
        (u32)dci | ((u32)slot << 16);
    __asm__ volatile ("" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* Context helpers                                                     */

static u32 *in_slot_ctx(u8 *ctx)
{
    return (u32 *)(ctx + 32);
}

static u32 *in_ep_ctx(u8 *ctx, int dci)
{
    int sz = xhci_csz ? 64 : 32;
    return (u32 *)(ctx + 32 + sz + sz * (dci - 1));
}

static u32 *in_ctrl_ctx(u8 *ctx)
{
    return (u32 *)ctx;
}

/* ------------------------------------------------------------------ */
/* HID report descriptor parsing                                       */

typedef struct {
    u16 page;
    i32 lmin, lmax;
    u32 rsize, rcount;
    u8 rid;
} HidGlobals;

static i32 sign_extend(u32 v, int bytes)
{
    if (bytes <= 0 || bytes >= 4)
        return (i32)v;
    u32 sign = 1u << (bytes * 8 - 1);
    v &= (1u << (bytes * 8)) - 1u;
    return (i32)((v ^ sign) - sign);
}

static void hid_add_field(UsbDev *d, u16 page, u16 usage, u16 umin,
                          u16 umax, u32 off, u32 size, i32 lmin, i32 lmax,
                          u8 rid, int array)
{
    if (d->nfields >= (int)(sizeof(d->fields) / sizeof(d->fields[0])))
        return;
    HidField *f = &d->fields[d->nfields++];
    f->page = page;
    f->usage = usage;
    f->umin = umin;
    f->umax = umax;
    f->off = off;
    f->size = (u8)(size > 32 ? 32 : size);
    f->rid = rid;
    f->sign = (lmin < 0);
    f->array = (u8)(array ? 1 : 0);
    f->lmin = lmin;
    f->lmax = lmax;
}

static void hid_parse(UsbDev *d, const u8 *desc, int len)
{
    HidGlobals g = { 0, 0, 0, 0, 0, 0 };
    HidGlobals stack[8];
    int sp = 0;
    u32 usages[16];
    int nusage = 0;
    u16 umin = 0, umax = 0;
    u16 umin_page = 0;
    u32 bitoff = 0;
    int have_rid = 0;

    d->nfields = 0;
    d->report_bits = 0;

    for (int i = 0; i + 1 < len; ) {
        u8 b = desc[i++];
        if (b == 0xFE) {                    /* long item */
            if (i + 2 > len)
                break;
            u8 dsize = desc[i];
            i += 2;
            i += dsize;
            continue;
        }
        int size = b & 3;
        if (size == 3) size = 4;
        int type = (b >> 2) & 3;
        int tag = (b >> 4) & 0x0F;
        if (i + size > len)
            break;

        u32 val = 0;
        for (int k = 0; k < size; k++)
            val |= (u32)desc[i + k] << (8 * k);
        i += size;

        if (type == 1) {                    /* global */
            switch (tag) {
            case 0x0: g.page = (u16)val; break;
            case 0x1: g.lmin = sign_extend(val, size); break;
            case 0x2: g.lmax = sign_extend(val, size); break;
            case 0x7: g.rsize = val > 32 ? 32 : val; break;
            case 0x8:
                g.rid = (u8)val;
                have_rid = 1;
                bitoff = 0;             /* each report ID has its own bits */
                break;
            case 0x9: g.rcount = val > 64 ? 64 : val; break;
            case 0xA:                       /* push */
                if (sp < 8) stack[sp++] = g;
                break;
            case 0xB:                       /* pop */
                if (sp > 0) g = stack[--sp];
                break;
            default: break;
            }
        } else if (type == 2) {             /* local */
            if (tag == 0x0) {
                u16 page = g.page;
                u16 usage = (u16)val;
                if (size == 4 && (val >> 16) != 0) {
                    page = (u16)(val >> 16);
                    usage = (u16)val;
                }
                if (nusage < 16)
                    usages[nusage++] = ((u32)page << 16) | usage;
            } else if (tag == 0x1 || tag == 0x2) {
                u16 page = g.page;
                u16 usage = (u16)val;
                if (size == 4 && (val >> 16) != 0) {
                    page = (u16)(val >> 16);
                    usage = (u16)val;
                }
                if (tag == 0x1) {
                    umin = usage;
                    umin_page = page;
                } else {
                    umax = usage;
                    if (umin_page == 0)
                        umin_page = page;
                }
            }
        } else {                            /* main */
            if (tag == 0x8) {               /* Input */
                int variable = (val & 2) != 0;
                for (u32 k = 0; k < g.rcount; k++) {
                    u16 up = g.page, usage = 0, fmin = 0, fmax = 0;
                    int array = 0;
                    array = variable ? 0 : 1;
                    if (nusage > 0) {
                        int idx = (k < (u32)nusage) ? (int)k : nusage - 1;
                        up = (u16)(usages[idx] >> 16);
                        usage = (u16)(usages[idx] & 0xFFFF);
                    } else if (umax >= umin) {
                        if (variable) {
                            up = umin_page;
                            usage = (u16)(umin + k);
                            if (usage > umax) usage = umax;
                        } else {
                            up = umin_page;
                            usage = umin;
                            fmin = umin;
                            fmax = umax;
                            array = 1;
                        }
                    }
                    if (g.rsize > 0)
                        hid_add_field(d, up, usage, fmin, fmax, bitoff,
                                      g.rsize, g.lmin, g.lmax, g.rid, array);
                    bitoff += g.rsize;
                }
                if (bitoff > (u32)d->report_bits)
                    d->report_bits = (int)bitoff;
                nusage = 0;
                umin = 0;
                umax = 0;
            } else if (tag == 0x9 || tag == 0xB) {  /* Output/Feature */
                nusage = 0;
                umin = 0;
                umax = 0;
            } else if (tag == 0xA) {        /* Collection */
                nusage = 0;
                umin = 0;
                umax = 0;
            } else if (tag == 0xC) {        /* End Collection */
                nusage = 0;
                umin = 0;
                umax = 0;
            }
        }
    }

    d->has_rid = have_rid;
    d->report_len = (d->report_bits + 7) / 8;
    if (d->report_len > 0 && d->has_rid)
        d->report_len++;
    if (d->report_len > 256)
        d->report_len = 256;
}

static i32 hid_get(const u8 *data, int len, const HidField *f)
{
    if (f->size == 0 || f->size > 32)
        return 0;
    u32 v = 0;
    for (u32 i = 0; i < f->size; i++) {
        u32 bit = f->off + i;
        if ((int)(bit >> 3) >= len)
            return 0;
        v |= (u32)((data[bit >> 3] >> (bit & 7)) & 1u) << i;
    }
    if (f->sign && f->size < 32) {
        u32 m = 1u << (f->size - 1);
        if (v & m)
            v |= ~((1u << f->size) - 1u);
    }
    return (i32)v;
}

static int hid_usage_is_key(u16 usage)
{
    return usage >= 0x04 && usage <= 0xE7;
}

static void hid_classify(UsbDev *d)
{
    int xf = -1, yf = -1, tf = -1, bf = -1, cf = -1;
    int page_x = 0;

    for (int i = 0; i < d->nfields; i++) {
        HidField *f = &d->fields[i];
        if ((f->page == 0x0D || f->page == 0x01) &&
            f->usage == 0x30 && xf < 0) {
            xf = i;
            page_x = f->page;
        } else if ((f->page == 0x0D || f->page == 0x01) &&
                   f->usage == 0x31 && yf < 0) {
            yf = i;
        } else if (f->page == 0x0D &&
                   (f->usage == 0x42 || f->usage == 0x45) && tf < 0) {
            tf = i;
        } else if (f->page == 0x09 && f->usage >= 1 && f->usage <= 3 &&
                   bf < 0) {
            bf = i;
        } else if (f->page == 0x0D && f->usage == 0x3A && cf < 0) {
            cf = i;                     /* contact count */
        }
    }

    {
        int relative = (xf >= 0 && yf >= 0 && page_x == 0x01 &&
                        d->fields[xf].sign && d->fields[yf].sign);
        if (xf >= 0 && yf >= 0 && !relative) {
            d->kind = USB_KIND_TOUCH;
            d->xf = xf;
            d->yf = yf;
            d->tf = (tf >= 0) ? tf : bf;
            d->cf = cf;
            return;
        }
    }

    /* Any page-0x07 field means keyboard/keypad. */
    for (int i = 0; i < d->nfields; i++) {
        HidField *kf = &d->fields[i];
        if (kf->page == 0x07 &&
            (hid_usage_is_key(kf->usage) ||
             (kf->umax >= 0x04 && kf->umin <= 0xE7))) {
            d->kind = USB_KIND_KEYBOARD;
            return;
        }
    }

    if (xf >= 0 && yf >= 0) {
        d->kind = USB_KIND_MOUSE;
        d->xf = xf;
        d->yf = yf;
        d->bf = bf;
        return;
    }

    d->kind = USB_KIND_NONE;
}

/* ------------------------------------------------------------------ */
/* Keyboard translation (HID usage -> PS/2 set 1)                      */

static int kbd_usage_code(u16 usage, int *ext)
{
    *ext = 0;
    switch (usage) {
    case 0x04: return 0x1E; case 0x05: return 0x30; case 0x06: return 0x2E;
    case 0x07: return 0x20; case 0x08: return 0x12; case 0x09: return 0x21;
    case 0x0A: return 0x22; case 0x0B: return 0x23; case 0x0C: return 0x17;
    case 0x0D: return 0x24; case 0x0E: return 0x25; case 0x0F: return 0x26;
    case 0x10: return 0x32; case 0x11: return 0x31; case 0x12: return 0x18;
    case 0x13: return 0x19; case 0x14: return 0x10; case 0x15: return 0x13;
    case 0x16: return 0x1F; case 0x17: return 0x14; case 0x18: return 0x16;
    case 0x19: return 0x2F; case 0x1A: return 0x11; case 0x1B: return 0x2D;
    case 0x1C: return 0x15; case 0x1D: return 0x2C;
    case 0x1E: return 0x02; case 0x1F: return 0x03; case 0x20: return 0x04;
    case 0x21: return 0x05; case 0x22: return 0x06; case 0x23: return 0x07;
    case 0x24: return 0x08; case 0x25: return 0x09; case 0x26: return 0x0A;
    case 0x27: return 0x0B;
    case 0x28: return 0x1C; case 0x29: return 0x01; case 0x2A: return 0x0E;
    case 0x2B: return 0x0F; case 0x2C: return 0x39; case 0x2D: return 0x0C;
    case 0x2E: return 0x0D; case 0x2F: return 0x1A; case 0x30: return 0x1B;
    case 0x31: return 0x2B; case 0x32: return -1;   case 0x33: return 0x27;
    case 0x34: return 0x28; case 0x35: return 0x29; case 0x36: return 0x33;
    case 0x37: return 0x34; case 0x38: return 0x35; case 0x39: return 0x3A;
    case 0x3A: return 0x3B; case 0x3B: return 0x3C; case 0x3C: return 0x3D;
    case 0x3D: return 0x3E; case 0x3E: return 0x3F; case 0x3F: return 0x40;
    case 0x40: return 0x41; case 0x41: return 0x42; case 0x42: return 0x43;
    case 0x43: return 0x44; case 0x44: return 0x57; case 0x45: return 0x58;
    case 0x49: *ext = 1; return 0x52;          /* insert */
    case 0x4A: *ext = 1; return 0x47;
    case 0x4B: *ext = 1; return 0x49;
    case 0x4C: *ext = 1; return 0x53;
    case 0x4D: *ext = 1; return 0x4F;
    case 0x4E: *ext = 1; return 0x51;
    case 0x4F: *ext = 1; return 0x4D;
    case 0x50: *ext = 1; return 0x4B;
    case 0x51: *ext = 1; return 0x50;
    case 0x52: *ext = 1; return 0x48;
    case 0x53: *ext = 1; return 0x45;          /* num lock */
    case 0x54: *ext = 1; return 0x35;
    case 0x55: *ext = 1; return 0x37;
    case 0x56: *ext = 1; return 0x4A;
    case 0x57: *ext = 1; return 0x4E;
    case 0x58: *ext = 1; return 0x1C;          /* keypad enter */
    case 0x59: *ext = 1; return 0x1F;
    case 0x5A: *ext = 1; return 0x3A;
    case 0x5B: *ext = 1; return 0x46;
    default: return -1;
    }
}

static void kbd_usage_event(u16 usage, int down)
{
    int ext = 0;
    int code = kbd_usage_code(usage, &ext);
    if (code < 0)
        return;
    if (ext)
        kbd_feed(0xE0);
    kbd_feed((u8)(code | (down ? 0 : 0x80)));
}

static void kbd_modifier_event(int bit, int down)
{
    int code = 0, ext = 0;
    switch (bit) {
    case 0: code = 0x1D; break;           /* left ctrl */
    case 1: code = 0x2A; break;           /* left shift */
    case 2: code = 0x38; break;           /* left alt */
    case 3: ext = 1; code = 0x5B; break;  /* left gui */
    case 4: ext = 1; code = 0x1D; break;  /* right ctrl */
    case 5: code = 0x36; break;           /* right shift */
    case 6: ext = 1; code = 0x38; break;  /* right alt */
    case 7: ext = 1; code = 0x5C; break;  /* right gui */
    default: return;
    }
    if (ext)
        kbd_feed(0xE0);
    kbd_feed((u8)(code | (down ? 0 : 0x80)));
}

/* ------------------------------------------------------------------ */
/* Report processing                                                   */

static void process_keyboard_report(UsbDev *d, const u8 *rep, int len)
{
    u8 keys[8];
    int n = 0;
    u8 mod = 0;

    if (rep == NULL || len <= 0)
        return;

    if (d->kind == USB_KIND_KEYBOARD && d->has_rid == 0 &&
        d->nfields == 0) {
        /* boot report: modifiers, reserved, six key codes */
        mod = rep[0];
        for (int i = 2; i < len && i < 8 && n < 8; i++) {
            if (rep[i] >= 0x04)
                keys[n++] = rep[i];
        }
    } else {
        for (int i = 0; i < d->nfields && n < 8; i++) {
            HidField *f = &d->fields[i];
            if (f->page != 0x07)
                continue;
            i32 v = hid_get(rep, len, f);
            if (v == 0)
                continue;
            if (!f->array && f->usage >= 0xE0) {
                if (f->usage <= 0xE7)
                    mod |= (u8)(1u << (f->usage - 0xE0));
            } else if (!f->array && f->usage >= 0x04) {
                keys[n++] = (u8)f->usage;
            } else if (v >= 0x04 && v <= 0xE7) {
                keys[n++] = (u8)v;          /* array item */
            }
        }
    }

    /* modifiers */
    u8 changed = mod ^ d->prev_mod;
    for (int b = 0; b < 8; b++) {
        if (changed & (1u << b))
            kbd_modifier_event(b, (mod >> b) & 1);
    }
    d->prev_mod = mod;

    /* keys */
    for (int i = 0; i < 8; i++) {
        u8 old = d->prev_keys[i];
        if (old == 0)
            continue;
        int found = 0;
        for (int k = 0; k < n; k++) {
            if (keys[k] == old)
                found = 1;
        }
        if (!found)
            kbd_usage_event(old, 0);
    }
    for (int k = 0; k < n; k++) {
        int found = 0;
        for (int i = 0; i < 8; i++) {
            if (d->prev_keys[i] == keys[k])
                found = 1;
        }
        if (!found)
            kbd_usage_event(keys[k], 1);
    }
    memset(d->prev_keys, 0, sizeof(d->prev_keys));
    for (int k = 0; k < n && k < 8; k++)
        d->prev_keys[k] = keys[k];
}

static void process_mouse_report(UsbDev *d, const u8 *rep, int len)
{
    if (rep == NULL || len <= 0)
        return;

    int dx = 0, dy = 0;
    u8 buttons = 0;

    if (d->nfields == 0) {
        /* boot mouse */
        if (len < 3)
            return;
        buttons = rep[0] & 0x07;
        dx = (i8)rep[1];
        dy = (i8)rep[2];
    } else {
        for (int i = 0; i < d->nfields; i++) {
            HidField *f = &d->fields[i];
            if (f->page == 0x09 && f->usage >= 1 && f->usage <= 3 &&
                hid_get(rep, len, f))
                buttons |= (u8)(1u << (f->usage - 1));
        }
        if (d->xf >= 0)
            dx = hid_get(rep, len, &d->fields[d->xf]);
        if (d->yf >= 0)
            dy = hid_get(rep, len, &d->fields[d->yf]);
    }
    mouse_usb_relative(dx, dy, buttons);
}

static void process_touch_report(UsbDev *d, const u8 *rep, int len)
{
    if (rep == NULL || len <= 0)
        return;
    if (d->xf < 0 || d->yf < 0)
        return;

    HidField *fx = &d->fields[d->xf];
    HidField *fy = &d->fields[d->yf];
    i32 x = hid_get(rep, len, fx);
    i32 y = hid_get(rep, len, fy);
    i32 down = 1;
    if (d->tf >= 0)
        down = hid_get(rep, len, &d->fields[d->tf]) ? 1 : 0;

    int w = (int)gfx.width;
    int h = (int)gfx.height;
    if (w <= 0 || h <= 0)
        return;

    i32 xmin = fx->lmin, xmax = fx->lmax;
    i32 ymin = fy->lmin, ymax = fy->lmax;
    if (xmax <= xmin) xmax = xmin + 1;
    if (ymax <= ymin) ymax = ymin + 1;

    int px = (int)((i64)(x - xmin) * (w - 1) / (xmax - xmin));
    int py = (int)((i64)(y - ymin) * (h - 1) / (ymax - ymin));
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= w) px = w - 1;
    if (py >= h) py = h - 1;

    mouse_touch_absolute(px, py, down);
}

static void process_report(UsbDev *d, int len)
{
    u8 *buf = report_buf[d - devs];
    const u8 *rep = buf;
    int rlen = len;

    if (rlen > 256)
        rlen = 256;

    if (d->has_rid) {
        if (rlen < 1 || buf[0] != (u8)d->rid)
            return;
        rep = buf + 1;
        rlen--;
    }

    switch (d->kind) {
    case USB_KIND_KEYBOARD:
        process_keyboard_report(d, rep, rlen);
        break;
    case USB_KIND_MOUSE:
        process_mouse_report(d, rep, rlen);
        break;
    case USB_KIND_TOUCH:
        process_touch_report(d, rep, rlen);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Transfers                                                           */

static void usb_arm(UsbDev *d)
{
    if (!d->used || d->dead || d->armed || d->dci <= 0)
        return;
    int len = d->xfer_len;
    if (len <= 0)
        len = d->mps > 0 ? d->mps : 8;
    u64 buf = phys(report_buf[d - devs]);
    ring_push(in_ring_mem[d - devs], &d->in_idx, &d->in_cyc,
              (u32)buf, (u32)(buf >> 32), (u32)len,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    doorbell_ep(d->slot, d->dci);
    d->armed = 1;
}

static void handle_transfer_event(const Trb *ev)
{
    int slot = (int)((ev->d3 >> 24) & 0xFF);
    int dci = (int)((ev->d3 >> 16) & 0x1F);
    int code = (int)((ev->d2 >> 24) & 0xFF);
    int residual = (int)(ev->d2 & 0xFFFFFF);

    for (int i = 0; i < USB_MAX_DEVS; i++) {
        UsbDev *d = &devs[i];
        if (!d->used || d->slot != slot || d->dci != dci)
            continue;
        d->armed = 0;
        if (code == 1 || code == 13) {
            int len = d->xfer_len - residual;
            if (len < 0) len = 0;
            if (len > 256) len = 256;
            process_report(d, len);
        } else {
            /* Stall or transaction error: stop polling this endpoint. */
            serial_write("usb: endpoint error ");
            serial_dec((u32)code);
            serial_write(" slot ");
            serial_dec((u32)slot);
            serial_write("\r\n");
            d->dead = 1;
            return;
        }
        usb_arm(d);
        return;
    }
}

static void dispatch_event(const Trb *ev)
{
    int type = (int)((ev->d3 >> 10) & 0x3F);
    if (type == TRB_TRANSFER_EVENT)
        handle_transfer_event(ev);
    else if (type == TRB_PORT_STATUS)
        need_scan = 1;
}

/* Control transfer on EP0.  Returns 0 on success. */
static int usb_control(UsbDev *d, u8 rt, u8 req, u16 val, u16 idx,
                       u16 len, void *buf)
{
    u8 setup[8];
    setup[0] = rt;
    setup[1] = req;
    setup[2] = (u8)val;
    setup[3] = (u8)(val >> 8);
    setup[4] = (u8)idx;
    setup[5] = (u8)(idx >> 8);
    setup[6] = (u8)len;
    setup[7] = (u8)(len >> 8);

    u32 s0 = (u32)setup[0] | ((u32)setup[1] << 8) |
             ((u32)setup[2] << 16) | ((u32)setup[3] << 24);
    u32 s1 = (u32)setup[4] | ((u32)setup[5] << 8) |
             ((u32)setup[6] << 16) | ((u32)setup[7] << 24);

    u32 trt = 0;
    if (len > 0)
        trt = (rt & 0x80) ? 3 : 2;

    ring_push(ep0_ring_mem[d - devs], &d->ep0_idx, &d->ep0_cyc,
              s0, s1, 8, TRB_TYPE(TRB_SETUP) | (trt << 16));

    if (len > 0) {
        u64 a = phys(buf);
        u32 flags = TRB_TYPE(TRB_DATA);
        if (rt & 0x80)
            flags |= TRB_DIR_IN;
        ring_push(ep0_ring_mem[d - devs], &d->ep0_idx, &d->ep0_cyc,
                  (u32)a, (u32)(a >> 32), len, flags);
    }

    /* Status stage: opposite direction of the data stage. */
    u32 sflags = TRB_TYPE(TRB_STATUS) | TRB_IOC;
    if (len == 0 || !(rt & 0x80))
        sflags |= TRB_DIR_IN;           /* status is IN for OUT/no-data */

    ring_push(ep0_ring_mem[d - devs], &d->ep0_idx, &d->ep0_cyc,
              0, 0, 0, sflags);
    doorbell_ep(d->slot, 1);

    Trb ev;
    u64 start = timer_ms();
    while (timer_ms() - start < 1000) {
        if (!event_poll(&ev)) {
            __asm__ volatile ("pause");
            continue;
        }
        int type = (int)((ev.d3 >> 10) & 0x3F);
        if (type == TRB_TRANSFER_EVENT) {
            int slot = (int)((ev.d3 >> 24) & 0xFF);
            int dci = (int)((ev.d3 >> 16) & 0x1F);
            if (slot == d->slot && dci == 1) {
                int code = (int)((ev.d2 >> 24) & 0xFF);
                return (code == 1 || code == 13) ? 0 : -code;
            }
            handle_transfer_event(&ev);
        } else {
            dispatch_event(&ev);
        }
    }
    return -1;
}

static int usb_get_descriptor(UsbDev *d, u8 type, u8 index, u16 lang,
                              void *buf, u16 len)
{
    return usb_control(d, 0x80, 6, (u16)((type << 8) | index), lang, len, buf);
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */

static int xhci_address_device(UsbDev *d, int bsr, int mps0, int entries)
{
    u8 *in = in_ctx_mem[d - devs];
    u8 *dev = dev_ctx_mem[d - devs];
    memset(in, 0, 2048);
    memset(dev, 0, 2048);

    u32 *icc = in_ctrl_ctx(in);
    icc[1] = (1u << 0) | (1u << 1);      /* add slot + EP0 */

    u32 *slot = in_slot_ctx(in);
    slot[0] = ((u32)d->speed << 20) | ((u32)entries << 27);
    slot[1] = (u32)d->port << 16;

    u32 *ep = in_ep_ctx(in, 1);
    ep[0] = 0;                           /* interval */
    ep[1] = (u32)mps0 | (3u << 16) | (4u << 18);  /* CErr=3, control */
    u64 ring = phys(ep0_ring_mem[d - devs]) + (u64)d->ep0_idx * 16;
    ring |= (d->ep0_cyc & 1u);
    ep[2] = (u32)ring;
    ep[3] = (u32)(ring >> 32);
    ep[4] = 8;                           /* average TRB length */

    dcbaa[d->slot] = phys(dev);

    u32 flags = TRB_TYPE(TRB_ADDRESS_DEVICE) | ((u32)d->slot << 24);
    if (bsr)
        flags |= (1u << 9);
    return xhci_cmd((u32)phys(in), (u32)(phys(in) >> 32), 0, flags, NULL);
}

static int xhci_configure_ep(UsbDev *d, int dci, int mps, int interval)
{
    u8 *in = in_ctx_mem[d - devs];
    memset(in, 0, 2048);

    u32 *icc = in_ctrl_ctx(in);
    icc[1] = (1u << 0) | (1u << dci);

    u32 *slot = in_slot_ctx(in);
    slot[0] = ((u32)d->speed << 20) | ((u32)dci << 27);
    slot[1] = (u32)d->port << 16;

    u32 *ep = in_ep_ctx(in, dci);
    ep[0] = (u32)interval << 16;
    ep[1] = (u32)mps | (3u << 16) | (7u << 18);   /* interrupt IN */
    u64 ring = phys(in_ring_mem[d - devs]);
    ep[2] = (u32)ring;
    ep[3] = (u32)(ring >> 32);
    ep[4] = (u32)mps;                    /* average TRB length */

    u32 flags = TRB_TYPE(TRB_CONFIGURE_EP) | ((u32)d->slot << 24);
    return xhci_cmd((u32)phys(in), (u32)(phys(in) >> 32), 0, flags, NULL);
}

static void xhci_disable_slot(int slot)
{
    xhci_cmd(0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((u32)slot << 24), NULL);
    if (slot >= 0 && slot < 64)
        dcbaa[slot] = 0;
}

/* Reset a root port and return its speed, or 0 on failure. */
static int xhci_reset_port(int port)
{
    u32 off = 0x400 + (u32)(port - 1) * 0x10;
    volatile u8 *pr = xhci_op + off;
    u32 p = *(volatile u32 *)pr;

    if (!(p & PORTSC_CCS))
        return 0;

    if (!(p & PORTSC_PP)) {
        *(volatile u32 *)pr = p | PORTSC_PP;
        timer_delay_ms(20);
        p = *(volatile u32 *)pr;
    }

    /* Reset and wait for the change bit. */
    *(volatile u32 *)pr = (p & ~PORTSC_RW1C) | PORTSC_PR;
    u64 start = timer_ms();
    while (timer_ms() - start < 500) {
        p = *(volatile u32 *)pr;
        if (p & PORTSC_PRC)
            break;
        __asm__ volatile ("pause");
    }
    if (!(p & PORTSC_PRC)) {
        serial_write("usb: port reset timeout\r\n");
        return 0;
    }

    /* Acknowledge all change bits. */
    *(volatile u32 *)pr = (p & ~PORTSC_PR) | PORTSC_CHANGE;

    start = timer_ms();
    while (timer_ms() - start < 500) {
        p = *(volatile u32 *)pr;
        if (p & PORTSC_PED)
            break;
        __asm__ volatile ("pause");
    }
    if (!(p & PORTSC_PED)) {
        serial_write("usb: port not enabled\r\n");
        return 0;
    }

    return (int)((p >> 10) & 0x0F);
}

static int usb_ep0_mps_for_speed(int speed)
{
    if (speed == 3)             /* high speed */
        return 64;
    if (speed >= 4)             /* super speed */
        return 512;
    if (speed == 2)             /* low speed */
        return 8;
    return 8;                   /* full speed, refined after descriptor */
}

static void usb_free_dev(UsbDev *d)
{
    if (d->used && d->slot)
        xhci_disable_slot(d->slot);
    memset(d, 0, sizeof(*d));
}

static int usb_parse_config(UsbDev *d, const u8 *cfg, int total,
                            int *cfg_value, int *iface_num,
                            int *subclass, int *protocol,
                            int *ep_addr, int *ep_mps, int *ep_interval)
{
    int in_iface = -1;
    int found = 0;
    int hid_iface = 0;

    *ep_addr = 0;
    *ep_mps = 0;
    *ep_interval = 0;
    *iface_num = 0;
    *subclass = 0;
    *protocol = 0;

    for (int i = 0; i + 2 <= total; ) {
        u8 blen = cfg[i];
        u8 type = cfg[i + 1];
        if (blen < 2 || i + blen > total)
            break;

        if (type == 4 && blen >= 9) {            /* interface */
            int num = cfg[i + 2];
            int alt = cfg[i + 3];
            int cls = cfg[i + 5];
            int sub = cfg[i + 6];
            int proto = cfg[i + 7];
            in_iface = (alt == 0 && cls == 3) ? num : -1;
            if (in_iface >= 0 && !found) {
                hid_iface = 1;
                *iface_num = num;
                *subclass = sub;
                *protocol = proto;
            }
        } else if (type == 5 && blen >= 7 && in_iface >= 0) {  /* endpoint */
            int addr = cfg[i + 2];
            int attr = cfg[i + 3];
            int mps = cfg[i + 4] | (cfg[i + 5] << 8);
            int interval = cfg[i + 6];
            if ((addr & 0x80) && ((attr & 3) == 3) && !found) {
                *ep_addr = addr;
                *ep_mps = mps & 0x7FF;
                *ep_interval = interval;
                found = 1;
                d->iface = *iface_num;
                d->kind = USB_KIND_NONE;    /* filled by HID parsing */
            }
        } else if (type == 2 && blen >= 9) {     /* config */
            *cfg_value = cfg[i + 5];
        }
        i += blen;
    }
    return hid_iface && found;
}

static void usb_scan(void);

static int usb_enum_port(int port, int speed)
{
    int slot = -1;
    int code = xhci_cmd(0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), (u32 *)&slot);
    if (code != 1 || slot <= 0 || slot >= max_slots) {
        serial_write("usb: enable slot failed\r\n");
        return 0;
    }

    int idx = -1;
    for (int i = 0; i < USB_MAX_DEVS; i++) {
        if (!devs[i].used) { idx = i; break; }
    }
    if (idx < 0) {
        xhci_disable_slot(slot);
        return 0;
    }

    UsbDev *d = &devs[idx];
    memset(d, 0, sizeof(*d));
    d->used = 1;
    d->slot = slot;
    d->port = port;
    d->speed = speed;
    d->ep0_cyc = 1;
    d->in_cyc = 1;
    d->dci = 0;
    ring_reset(ep0_ring_mem[idx]);
    ring_reset(in_ring_mem[idx]);

    int mps0 = usb_ep0_mps_for_speed(speed);

    code = xhci_address_device(d, 1, mps0, 1);
    if (code != 1) {
        serial_write("usb: address (bsr) failed ");
        serial_dec((u32)code);
        serial_write("\r\n");
        usb_free_dev(d);
        return 0;
    }

    u8 dev_desc[18];
    code = usb_get_descriptor(d, 1, 0, 0, dev_desc, 8);
    if (code != 0) {
        serial_write("usb: device descriptor failed\r\n");
        usb_free_dev(d);
        return 0;
    }
    int bmps = dev_desc[7];
    if (speed == 1 && bmps >= 8 && bmps <= 64)
        mps0 = bmps;

    code = xhci_address_device(d, 0, mps0, 1);
    if (code != 1) {
        serial_write("usb: set address failed ");
        serial_dec((u32)code);
        serial_write("\r\n");
        usb_free_dev(d);
        return 0;
    }

    memset(dev_desc, 0, sizeof(dev_desc));
    code = usb_get_descriptor(d, 1, 0, 0, dev_desc, sizeof(dev_desc));
    if (code != 0) {
        usb_free_dev(d);
        return 0;
    }

    memset(cfg_desc, 0, sizeof(cfg_desc));
    code = usb_get_descriptor(d, 2, 0, 0, cfg_desc, 9);
    if (code != 0) {
        usb_free_dev(d);
        return 0;
    }
    int total = cfg_desc[2] | (cfg_desc[3] << 8);
    if (total < 9 || total > (int)sizeof(cfg_desc))
        total = (int)sizeof(cfg_desc);

    code = usb_get_descriptor(d, 2, 0, 0, cfg_desc, (u16)total);
    if (code != 0) {
        usb_free_dev(d);
        return 0;
    }

    int cfg_value = 1, iface = 0, subclass = 0, protocol = 0;
    int ep_addr = 0, ep_mps = 0, ep_interval = 0;
    int has_hid = usb_parse_config(d, cfg_desc, total, &cfg_value, &iface,
                                   &subclass, &protocol,
                                   &ep_addr, &ep_mps, &ep_interval);
    if (!has_hid || ep_addr == 0) {
        serial_write("usb: device has no HID interrupt endpoint\r\n");
        usb_free_dev(d);
        return 0;
    }

    code = usb_control(d, 0x00, 9, (u16)cfg_value, 0, 0, NULL);
    if (code != 0) {
        serial_write("usb: set configuration failed\r\n");
        usb_free_dev(d);
        return 0;
    }

    memset(hid_desc, 0, sizeof(hid_desc));
    code = usb_control(d, 0x81, 6, 0x2200, (u16)iface, 1024, hid_desc);
    /* Some devices reject a multi-packet report request; retry with 512. */
    if (code != 0)
        code = usb_control(d, 0x81, 6, 0x2200, (u16)iface, 512, hid_desc);
    if (code != 0) {
        serial_write("usb: HID report descriptor failed\r\n");
        usb_free_dev(d);
        return 0;
    }

    hid_parse(d, hid_desc, 1024);
    hid_classify(d);
    if ((d->kind == USB_KIND_TOUCH || d->kind == USB_KIND_MOUSE) &&
        d->xf >= 0) {
        d->rid = d->fields[d->xf].rid;
    } else if (d->kind == USB_KIND_KEYBOARD) {
        for (int i = 0; i < d->nfields; i++) {
            if (d->fields[i].page == 0x07) {
                d->rid = d->fields[i].rid;
                break;
            }
        }
    }
    if (d->kind == USB_KIND_NONE) {
        serial_write("usb: unsupported HID device\r\n");
        usb_free_dev(d);
        return 0;
    }

    /* HID boot subclasses: switch to the boot report format. */
    if (subclass == 1 && protocol != 0) {
        usb_control(d, 0x21, 0x0B, 0, (u16)iface, 0, NULL);
        d->nfields = 0;
        d->has_rid = 0;
        if (d->kind == USB_KIND_KEYBOARD)
            d->report_len = 8;
        else if (d->kind == USB_KIND_MOUSE)
            d->report_len = 3;
    }
    usb_control(d, 0x21, 0x0A, 0, (u16)iface, 0, NULL);  /* SET_IDLE */

    int ep_num = ep_addr & 0x0F;
    int dci = ep_num * 2 + ((ep_addr & 0x80) ? 1 : 0);
    int interval = ep_interval;
    if (speed == 1 || speed == 2) {
        int shift = ep_interval > 0 ? ep_interval - 1 : 0;
        if (shift > 10) shift = 10;
        interval = 1 << shift;
    }
    if (interval < 1) interval = 1;
    if (interval > 255) interval = 255;

    code = xhci_configure_ep(d, dci, ep_mps, interval);
    if (code != 1) {
        serial_write("usb: configure endpoint failed ");
        serial_dec((u32)code);
        serial_write("\r\n");
        usb_free_dev(d);
        return 0;
    }

    d->dci = dci;
    d->mps = ep_mps > 0 ? ep_mps : 8;
    d->xfer_len = d->mps;
    if (d->report_len > d->xfer_len)
        d->xfer_len = d->report_len;
    if (d->xfer_len < 8)
        d->xfer_len = 8;
    if (d->xfer_len > 256)
        d->xfer_len = 256;

    memset(report_buf[idx], 0, 256);
    d->armed = 0;
    usb_arm(d);

    const char *what = "HID";
    if (d->kind == USB_KIND_KEYBOARD) what = "keyboard";
    else if (d->kind == USB_KIND_MOUSE) what = "mouse";
    else if (d->kind == USB_KIND_TOUCH) what = "touchscreen";
    serial_write("usb: ");
    serial_write(what);
    serial_write(" on port ");
    serial_dec((u32)port);
    serial_write(" speed ");
    serial_dec((u32)speed);
    serial_write("\r\n");

    dev_count++;
    return 1;
}

static void usb_scan(void)
{
    for (int port = 1; port <= max_ports && port <= 32; port++) {
        u32 off = 0x400 + (u32)(port - 1) * 0x10;
        u32 p = *(volatile u32 *)(xhci_op + off);
        int have = 0;

        for (int i = 0; i < USB_MAX_DEVS; i++) {
            if (devs[i].used && devs[i].port == port)
                have = 1;
        }

        if (!(p & PORTSC_CCS)) {
            if (have) {
                for (int i = 0; i < USB_MAX_DEVS; i++) {
                    if (devs[i].used && devs[i].port == port) {
                        serial_write("usb: device removed\r\n");
                        usb_free_dev(&devs[i]);
                        if (dev_count > 0)
                            dev_count--;
                    }
                }
            }
            continue;
        }

        /* Clear stale connect/change bits on unused ports. */
        if (!have && (p & PORTSC_CHANGE))
            *(volatile u32 *)(xhci_op + off) =
                (p & ~PORTSC_PR) | PORTSC_CHANGE;

        if (have)
            continue;

        int speed = xhci_reset_port(port);
        if (speed <= 0)
            continue;
        usb_enum_port(port, speed);
    }
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */

static int xhci_bios_handoff(void)
{
    u32 hcc = rr32(XCAP_HCCPARAMS1);
    u32 xoff = ((hcc >> 16) & 0xFFFF) * 4;
    if (xoff == 0)
        return 1;

    volatile u8 *base = xhci_mmio + xoff;
    u32 rel = 0;
    for (int guard = 0; guard < 32; guard++) {
        u32 cap = *(volatile u32 *)(base + rel);
        u8 id = (u8)(cap & 0xFF);
        if (id == 1) {                       /* USB Legacy Support */
            volatile u8 *ctl = base + rel;
            u32 v = *(volatile u32 *)ctl;
            *(volatile u32 *)ctl = v | (1u << 24);   /* OS owned */
            u64 start = timer_ms();
            while (timer_ms() - start < 1000) {
                if (!(*(volatile u32 *)ctl & (1u << 16)))  /* BIOS owned */
                    break;
                __asm__ volatile ("pause");
            }
            u32 lc = *(volatile u32 *)(ctl + 4);
            lc &= ~0x0000000Fu;
            *(volatile u32 *)(ctl + 4) = lc;
            return 1;
        }
        u32 next = (cap >> 8) & 0xFF;
        if (next == 0)
            break;
        rel += next * 4;
    }
    return 1;
}

static int xhci_init_controller(int bus, int dev, int func)
{
    u32 bar = pci_read32(bus, dev, func, PCI_BAR0);
    u64 addr;
    if (bar & 1) {
        serial_write("usb: xHCI has an I/O BAR, skipping\r\n");
        return 0;
    }
    if ((bar & 0x6) == 0x4) {
        u32 hi = pci_read32(bus, dev, func, PCI_BAR1);
        addr = (u64)(bar & ~0xFu) | ((u64)hi << 32);
    } else {
        addr = (u64)(bar & ~0xFu);
    }
    if (addr == 0 || addr >= 0x100000000ull) {
        serial_write("usb: xHCI BAR outside identity map\r\n");
        return 0;
    }

    u16 cmd = pci_read16(bus, dev, func, PCI_COMMAND);
    pci_write16(bus, dev, func, PCI_COMMAND, (u16)(cmd | 0x0006));

    xhci_mmio = (volatile u8 *)(uintptr_t)addr;
    u32 cap = rr32(XCAP_CAPLENGTH);
    u32 caplen = cap & 0xFF;
    u32 db = rr32(XCAP_DBOFF) & ~0x3u;
    u32 rt = rr32(XCAP_RTSOFF) & ~0x1Fu;
    xhci_op = xhci_mmio + caplen;
    xhci_db = xhci_mmio + db;
    xhci_rt = xhci_mmio + rt;

    u32 hcc = rr32(XCAP_HCCPARAMS1);
    xhci_csz = (hcc >> 2) & 1;
    u32 hcs1 = rr32(XCAP_HCSPARAMS1);
    max_ports = (int)((hcs1 >> 24) & 0xFF);
    max_slots = (int)(hcs1 & 0xFF);
    if (max_slots > 64) max_slots = 64;
    if (max_slots < 1) max_slots = 1;

    serial_write("usb: xHCI ports ");
    serial_dec((u32)max_ports);
    serial_write(" slots ");
    serial_dec((u32)max_slots);
    serial_write("\r\n");

    xhci_bios_handoff();

    /* Halt, then reset. */
    u32 sc = *(volatile u32 *)(xhci_op + XOP_USBCMD);
    *(volatile u32 *)(xhci_op + XOP_USBCMD) = sc & ~USBCMD_RS;
    u64 start = timer_ms();
    while (timer_ms() - start < 200) {
        if (*(volatile u32 *)(xhci_op + XOP_USBSTS) & USBSTS_HCH)
            break;
        __asm__ volatile ("pause");
    }

    *(volatile u32 *)(xhci_op + XOP_USBCMD) = USBCMD_HCRST;
    start = timer_ms();
    while (timer_ms() - start < 1000) {
        if (!(*(volatile u32 *)(xhci_op + XOP_USBCMD) & USBCMD_HCRST))
            break;
        __asm__ volatile ("pause");
    }
    start = timer_ms();
    while (timer_ms() - start < 1000) {
        if (!(*(volatile u32 *)(xhci_op + XOP_USBSTS) & USBSTS_CNR))
            break;
        __asm__ volatile ("pause");
    }

    *(volatile u32 *)(xhci_op + XOP_USBSTS) = USBSTS_EINT |
                                              0x00000004u |
                                              0x00000010u;

    ring_reset(cmd_ring);
    memset(event_ring, 0, sizeof(event_ring));
    memset(dcbaa, 0, sizeof(dcbaa));
    memset(erst, 0, sizeof(erst));
    cmd_idx = 0;
    cmd_cycle = 1;
    ev_idx = 0;
    ev_cycle = 1;

    u64 dcbaa_phys = phys(dcbaa);
    ww64(XOP_DCBAAP, dcbaa_phys);

    u32 cfg = *(volatile u32 *)(xhci_op + XOP_CONFIG);
    cfg &= ~0xFFu;
    cfg |= (u32)max_slots;
    *(volatile u32 *)(xhci_op + XOP_CONFIG) = cfg;

    ww64(XOP_CRCR, phys(cmd_ring) | 1u);

    u64 erst_phys = phys(erst);
    *(u32 *)(erst + 0) = (u32)phys(event_ring);
    *(u32 *)(erst + 4) = (u32)(phys(event_ring) >> 32);
    *(u32 *)(erst + 8) = EVENT_TRBS;
    *(u32 *)(erst + 12) = 0;

    *(volatile u32 *)(xhci_rt + XIR0_ERSTSZ) = 1;
    *(volatile u64 *)(xhci_rt + XIR0_ERSTBA) = erst_phys;
    *(volatile u64 *)(xhci_rt + XIR0_ERDP) = phys(event_ring);
    *(volatile u32 *)(xhci_rt + XIR0_IMAN) &= ~1u;

    *(volatile u32 *)(xhci_op + XOP_USBCMD) = USBCMD_RS;
    start = timer_ms();
    while (timer_ms() - start < 500) {
        if (!(*(volatile u32 *)(xhci_op + XOP_USBSTS) & USBSTS_HCH))
            break;
        __asm__ volatile ("pause");
    }

    xhci_ok = 1;
    return 1;
}

void usb_init(void)
{
    int bus = 0, dev = 0, func = 0;
    memset(devs, 0, sizeof(devs));
    dev_count = 0;
    xhci_ok = 0;

    if (!pci_find_class(XHCI_CLASS, XHCI_SUB, XHCI_PROGIF,
                        &bus, &dev, &func) &&
        !pci_find_class(XHCI_CLASS, XHCI_SUB, 0xFF, &bus, &dev, &func)) {
        serial_write("usb: no xHCI controller\r\n");
        return;
    }

    serial_write("usb: xHCI at ");
    serial_dec((u32)bus);
    serial_write(":");
    serial_dec((u32)dev);
    serial_write(".");
    serial_dec((u32)func);
    serial_write("\r\n");

    if (!xhci_init_controller(bus, dev, func)) {
        serial_write("usb: controller init failed\r\n");
        return;
    }

    usb_scan();
    serial_write("usb: ");
    serial_dec((u32)dev_count);
    serial_write(" input device(s)\r\n");
}

void usb_poll(void)
{
    if (!xhci_ok)
        return;

    Trb ev;
    for (int guard = 0; guard < 256; guard++) {
        if (!event_poll(&ev))
            break;
        dispatch_event(&ev);
    }

    if (need_scan) {
        need_scan = 0;
        usb_scan();
    }

    for (int i = 0; i < USB_MAX_DEVS; i++) {
        UsbDev *d = &devs[i];
        if (d->used && !d->dead && !d->armed)
            usb_arm(d);
    }
}

int usb_present(void)
{
    return dev_count;
}
