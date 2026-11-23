/* ACPI shutdown and reset.
 *
 * There is no paging, so the firmware tables are simply read through their
 * physical addresses. We walk EBDA/BIOS memory for the RSDP, follow RSDT to
 * the FADT, pull the S5 sleep type out of the DSDT (a very small AML parser
 * that understands the _S5_ package), and finally write SLP_TYP|SLP_EN to the
 * PM1 control register. If any step fails the caller shows a safe-to-switch
 * off message rather than guessing.
 */
#include "power.h"
#include "types.h"
#include "io.h"
#include "string.h"
#include "timer.h"
#include "serial.h"

#define PM1_CNT_SCI_EN  0x0001u
#define PM1_CNT_SLP_EN  0x2000u

static u32 pm1a_cnt;
static u32 pm1b_cnt;
static u8  pm1_cnt_len;
static u32 slp_typ_a = 5;
static u32 slp_typ_b = 5;
static int acpi_ready;

static u8 checksum8(const u8 *p, u32 len)
{
    u8 sum = 0;
    for (u32 i = 0; i < len; i++)
        sum = (u8)(sum + p[i]);
    return sum;
}

static int plausible(uintptr_t addr, u32 len)
{
    if (addr < 0x400u || len < 36u || len >= 0x00100000u)
        return 0;
    return (u64)addr + len > (u64)addr;
}

static const u8 *find_rsdp(void)
{
    /* The EBDA segment pointer lives at 0x40E in the BIOS data area. */
    u16 seg;
    __asm__ volatile ("movw 0x40E, %0" : "=r"(seg));
    uintptr_t ebda = (uintptr_t)seg << 4;
    const u8 *p;

    if (ebda >= 0x80000u && ebda < 0xA0000u) {
        for (u32 off = 0; off + 20 <= 1024; off += 16) {
            p = (const u8 *)(ebda + off);
            if (memcmp(p, "RSD PTR ", 8) == 0 && checksum8(p, 20) == 0)
                return p;
        }
    }

    for (u32 off = 0; off + 20 <= 0x10000u; off += 16) {
        p = (const u8 *)((uintptr_t)0xF0000u + off);
        if (memcmp(p, "RSD PTR ", 8) == 0 && checksum8(p, 20) == 0)
            return p;
    }
    return 0;
}

/* Walk an RSDT (32-bit entries) or XSDT (64-bit entries). */
static const u8 *find_table(const u8 *sdt, const char *sig, int extended)
{
    u32 len = *(const u32 *)(sdt + 4);
    if (!plausible((uintptr_t)sdt, len) || checksum8(sdt, len) != 0)
        return 0;

    u32 width = extended ? 8u : 4u;
    u32 entries = (len - 36u) / width;
    for (u32 i = 0; i < entries; i++) {
        uintptr_t addr;
        if (extended) {
            addr = (uintptr_t)(*(const u64 *)(sdt + 36u + i * 8u));
            if (!addr)
                continue;
        } else {
            addr = (uintptr_t)(*(const u32 *)(sdt + 36u + i * 4u));
            if (addr < 0x400u)
                continue;
        }
        const u8 *t = (const u8 *)addr;
        if (memcmp(t, sig, 4) != 0)
            continue;
        u32 tlen = *(const u32 *)(t + 4);
        if (!plausible(addr, tlen) || checksum8(t, tlen) != 0)
            continue;
        return t;
    }
    return 0;
}

static int aml_int(const u8 *p, const u8 *end, u32 *out)
{
    if (p >= end)
        return 0;
    if (*p == 0x00) { *out = 0; return 1; }
    if (*p == 0x01) { *out = 1; return 1; }
    if (*p == 0x0A) {
        if (p + 1 >= end) return 0;
        *out = p[1];
        return 2;
    }
    if (*p == 0x0B) {
        if (p + 2 >= end) return 0;
        *out = (u32)p[1] | ((u32)p[2] << 8);
        return 3;
    }
    if (*p == 0x0C) {
        if (p + 4 >= end) return 0;
        *out = (u32)p[1] | ((u32)p[2] << 8) | ((u32)p[3] << 16) |
               ((u32)p[4] << 24);
        return 5;
    }
    if (*p <= 0x7F) {
        *out = *p;
        return 1;
    }
    return 0;
}

static int aml_pkg_len(const u8 *p, const u8 *end, u32 *out)
{
    if (p >= end)
        return 0;
    u8 lead = *p;
    int extra = lead >> 6;
    if (extra == 0) {
        *out = lead & 0x3F;
        return 1;
    }
    if (p + extra >= end)
        return 0;
    u32 v = lead & 0x0F;
    for (int i = 0; i < extra; i++)
        v |= (u32)p[1 + i] << (4 + 8 * i);
    *out = v;
    return 1 + extra;
}

/* Look for _S5_ in the DSDT and decode its two sleep types. */
static void parse_s5(const u8 *dsdt)
{
    if (!dsdt)
        return;
    u32 len = *(const u32 *)(dsdt + 4);
    if (!plausible((uintptr_t)dsdt, len) || checksum8(dsdt, len) != 0)
        return;

    const u8 *end = dsdt + len;
    for (const u8 *p = dsdt + 36; p + 6 < end; p++) {
        if (memcmp(p, "_S5_", 4) != 0)
            continue;
        const u8 *q = p + 4;
        if (q >= end || *q != 0x12)            /* PackageOp */
            continue;
        q++;
        u32 pkg;
        int used = aml_pkg_len(q, end, &pkg);
        if (!used)
            continue;
        q += used;
        if (q >= end)
            continue;
        q++;                                    /* element count */
        u32 a, b;
        int n = aml_int(q, end, &a);
        if (!n)
            continue;
        q += n;
        if (!aml_int(q, end, &b))
            b = a;
        slp_typ_a = a & 7;
        slp_typ_b = b & 7;
        serial_write("acpi: _S5_ sleep type ");
        serial_dec(slp_typ_a);
        serial_write("\r\n");
        return;
    }
}

void power_init(u64 acpi_rsdp)
{
    acpi_ready = 0;
    const u8 *rsdp = acpi_rsdp ? (const u8 *)(uintptr_t)acpi_rsdp
                               : find_rsdp();
    if (!rsdp) {
        serial_write("acpi: no RSDP\r\n");
        return;
    }
    if (memcmp(rsdp, "RSD PTR ", 8) != 0 || checksum8(rsdp, 20) != 0) {
        serial_write("acpi: bad RSDP\r\n");
        return;
    }

    /* ACPI 2.0+ carries a 64-bit XSDT pointer; fall back to the 32-bit
       RSDT on older firmware. */
    const u8 *sdt = 0;
    int extended = 0;
    if (rsdp[15] >= 2) {
        u64 xsdt_addr = *(const u64 *)(rsdp + 24);
        if (xsdt_addr && *(const u32 *)(rsdp + 20) > 0) {
            sdt = (const u8 *)(uintptr_t)xsdt_addr;
            extended = 1;
        }
    }
    if (!sdt || memcmp(sdt, "XSDT", 4) != 0) {
        u32 rsdt_addr = *(const u32 *)(rsdp + 16);
        if (rsdt_addr < 0x400u) {
            serial_write("acpi: bad RSDT address\r\n");
            return;
        }
        sdt = (const u8 *)(uintptr_t)rsdt_addr;
        extended = 0;
        if (memcmp(sdt, "RSDT", 4) != 0) {
            serial_write("acpi: bad RSDT\r\n");
            return;
        }
    }
    const u8 *fadt = find_table(sdt, "FACP", extended);
    if (!fadt) {
        serial_write("acpi: no FADT\r\n");
        return;
    }

    pm1a_cnt = *(const u32 *)(fadt + 52);
    pm1b_cnt = *(const u32 *)(fadt + 56);
    pm1_cnt_len = fadt[77];
    if (pm1_cnt_len < 1 || pm1_cnt_len > 4)
        pm1_cnt_len = 2;
    if (pm1a_cnt < 0x400u) {
        serial_write("acpi: PM1 control missing\r\n");
        return;
    }

    uintptr_t dsdt_addr = 0;
    u32 fadt_len = *(const u32 *)(fadt + 4);
    if (fadt_len >= 148)
        dsdt_addr = (uintptr_t)(*(const u64 *)(fadt + 140));
    if (!dsdt_addr)
        dsdt_addr = (uintptr_t)(*(const u32 *)(fadt + 88));
    parse_s5((const u8 *)dsdt_addr);
    acpi_ready = 1;
    serial_write("acpi: PM1a ");
    serial_hex(pm1a_cnt);
    serial_write(" PM1b ");
    serial_hex(pm1b_cnt);
    serial_write("\r\n");
}

int power_available(void)
{
    return acpi_ready;
}

static void write_pm1(u32 port, u32 value)
{
    if (pm1_cnt_len >= 4)
        outl((u16)port, value);
    else if (pm1_cnt_len >= 2)
        outw((u16)port, (u16)value);
    else
        outb((u16)port, (u8)value);
}

int power_shutdown(void)
{
    if (acpi_ready && pm1a_cnt) {
        u32 a = (slp_typ_a << 10) | PM1_CNT_SLP_EN | PM1_CNT_SCI_EN;
        write_pm1(pm1a_cnt, a);
        if (pm1b_cnt)
            write_pm1(pm1b_cnt,
                      (slp_typ_b << 10) | PM1_CNT_SLP_EN | PM1_CNT_SCI_EN);

        /* Give the firmware a moment to cut the rail. If we are still
           executing, the request did not take. */
        timer_delay_ms(700);
    }

    /* Common chipset shortcuts, harmless when absent. */
    outw(0x604, 0x2000);            /* QEMU/Bochs ACPI PM */
    outw(0xB004, 0x2000);           /* older Bochs */
    timer_delay_ms(250);
    return 0;
}

void power_reboot(void)
{
    serial_write("power: reboot\r\n");
    cli();

    for (u32 i = 0; i < 100000u && (inb(0x64) & 0x02); i++) { }
    outb(0x64, 0xFE);

    for (volatile u32 i = 0; i < 2000000u; i++) { }
    outb(0xCF9, 0x06);

    struct __attribute__((packed)) { u16 limit; u64 base; } null_idt = { 0, 0 };
    __asm__ volatile ("lidt %0" : : "m"(null_idt));
    __asm__ volatile ("int $0x03");

    for (;;)
        hlt();
}
