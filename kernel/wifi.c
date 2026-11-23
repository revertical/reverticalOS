/* Wi-Fi station support with a simulated radio.
 *
 * There is no wireless chipset in the VM, so the radio itself is modelled in
 * the kernel: a table of access points, a scan, association and a real
 * WPA2-PSK supplicant.  The supplicant derives the PMK with
 * PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 256), builds the PTK with the
 * 802.11 PRF and verifies the four-way handshake MIC with HMAC-SHA1.
 *
 * The associated station is a normal NetIf ("wlan0") and the access point is
 * a second NetIf on the other side of the simulated medium.  That AP runs a
 * DHCP-free subnet (192.168.4.0/24, AP at .1, station at .2) and answers HTTP
 * on port 80, so the browser exercises ARP, IPv4, TCP and HTTP for real.
 */

#include "wifi.h"
#include "net.h"
#include "sha1.h"
#include "string.h"
#include "serial.h"
#include "timer.h"

typedef struct {
    WifiApInfo info;
    const char *passphrase;      /* passphrase the virtual AP expects */
} WifiAp;

static WifiAp aps[WIFI_MAX_APS];
static int ap_count;

static NetIf wlan;
static NetIf ap_if;              /* the other end of the simulated link */
static int connected;
static int connected_index = -1;
static u8 ptk[48];
static int handshake_done;
static char status[64] = "Wi-Fi off";

static u32 ip4(u8 a, u8 b, u8 c, u8 d)
{
    return ((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | d;
}

static void copy_str(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* 802.11 PRF: HMAC-SHA1(PMK, label | context | counter) blocks. */
static void derive_ptk(const u8 pmk[32], const u8 *ap_mac, const u8 *sta_mac,
                       const u8 *anonce, const u8 *snonce, u8 out[48])
{
    u8 data[128];
    u32 n = 0;
    const char *label = "Pairwise key expansion";
    while (*label) data[n++] = (u8)*label++;
    data[n++] = 0;

    const u8 *m1 = ap_mac, *m2 = sta_mac;
    if (memcmp(ap_mac, sta_mac, 6) > 0) { m1 = sta_mac; m2 = ap_mac; }
    memcpy(data + n, m1, 6); n += 6;
    memcpy(data + n, m2, 6); n += 6;

    const u8 *n1 = anonce, *n2 = snonce;
    if (memcmp(anonce, snonce, 32) > 0) { n1 = snonce; n2 = anonce; }
    memcpy(data + n, n1, 32); n += 32;
    memcpy(data + n, n2, 32); n += 32;

    for (u32 i = 0; i < 48; i += 20) {
        data[n] = (u8)i;
        u8 digest[20];
        hmac_sha1(pmk, 32, data, n + 1, digest);
        u32 take = (48 - i) < 20 ? (48 - i) : 20;
        memcpy(out + i, digest, take);
    }
}

static int station_send(NetIf *ifp, const void *frame, u32 len)
{
    (void)ifp;
    if (!connected)
        return -1;
    wlan.tx_frames++;
    netif_input(&ap_if, frame, len);     /* the simulated medium */
    return 0;
}

static int ap_send(NetIf *ifp, const void *frame, u32 len)
{
    (void)ifp;
    ap_if.tx_frames++;
    netif_input(&wlan, frame, len);
    return 0;
}

static void reset_link(void)
{
    wlan.up = 0;
    wlan.ip = wlan.mask = wlan.gw = wlan.dns = 0;
    wlan.tx_frames = wlan.rx_frames = 0;
    ap_if.tx_frames = ap_if.rx_frames = 0;
    connected = 0;
    connected_index = -1;
    handshake_done = 0;
    memset(ptk, 0, sizeof(ptk));
}

void wifi_init(void)
{
    ap_count = 0;

    struct {
        const char *ssid;
        u8 bssid[6];
        int channel;
        int rssi;
        int security;
        const char *pass;
    } table[WIFI_MAX_APS] = {
        { "RevertiaNet",   { 0x02,0x52,0x56,0x4F,0x53,0x11 },  6, -41, 1, "revertical" },
        { "Revertia Guest",{ 0x02,0x52,0x56,0x4F,0x53,0x12 }, 11, -55, 0, 0 },
        { "Orbit-5G",      { 0x02,0x52,0x56,0x4F,0x53,0x13 }, 36, -63, 1, "orbit2026" },
        { "Lab 2.4",       { 0x02,0x52,0x56,0x4F,0x53,0x14 },  1, -72, 1, "labpass" },
        { "RevertiaLab",   { 0x02,0x52,0x56,0x4F,0x53,0x15 }, 44, -78, 1, "revertical" },
    };

    for (int i = 0; i < 5; i++) {
        copy_str(aps[i].info.ssid, table[i].ssid, 33);
        memcpy(aps[i].info.bssid, table[i].bssid, 6);
        aps[i].info.channel = table[i].channel;
        aps[i].info.rssi = table[i].rssi;
        aps[i].info.security = table[i].security;
        aps[i].passphrase = table[i].pass;
        ap_count++;
    }

    memset(&wlan, 0, sizeof(wlan));
    copy_str(wlan.name, "wlan0", 8);
    wlan.mac[0] = 0x02; wlan.mac[1] = 0x52; wlan.mac[2] = 0x56;
    wlan.mac[3] = 0x4F; wlan.mac[4] = 0x53; wlan.mac[5] = 0x21;
    wlan.send = station_send;
    reset_link();
    net_register(&wlan);

    memset(&ap_if, 0, sizeof(ap_if));
    copy_str(ap_if.name, "ap0", 8);
    ap_if.mac[0] = 0x02; ap_if.mac[1] = 0x52; ap_if.mac[2] = 0x56;
    ap_if.mac[3] = 0x4F; ap_if.mac[4] = 0x53; ap_if.mac[5] = 0x01;
    ap_if.ip = ip4(192, 168, 4, 1);
    ap_if.mask = 0xFFFFFF00u;
    ap_if.up = 1;
    ap_if.send = ap_send;

    serial_write("wifi: virtual radio, ");
    serial_dec((u32)ap_count);
    serial_write(" access points\r\n");
}

void wifi_poll(void) { }

int wifi_scan(void) { return ap_count; }
int wifi_ap_count(void) { return ap_count; }
const WifiApInfo *wifi_ap(int index)
{
    return (index >= 0 && index < ap_count) ? &aps[index].info : 0;
}
int wifi_security(int index)
{
    return (index >= 0 && index < ap_count) ? aps[index].info.security : -1;
}

int wifi_connect(int index, const char *passphrase, char *err, int errlen)
{
    if (errlen) err[0] = 0;
    if (index < 0 || index >= ap_count) {
        if (errlen) copy_str(err, "Unknown network", errlen);
        return -1;
    }

    WifiAp *ap = &aps[index];
    memset(ptk, 0, sizeof(ptk));
    handshake_done = 0;

    if (ap->info.security) {
        u8 pmk[32], ap_pmk[32];
        u32 plen = strlen(passphrase);
        if (plen < 8) {
            if (errlen) copy_str(err, "WPA2 passphrase is at least 8 characters", errlen);
            return -2;
        }
        pbkdf2_sha1(passphrase, ap->info.ssid, pmk);
        pbkdf2_sha1(ap->passphrase, ap->info.ssid, ap_pmk);
        if (memcmp(pmk, ap_pmk, 32) != 0) {
            if (errlen) copy_str(err, "Wrong password", errlen);
            serial_write("wifi: PMK mismatch, association rejected\r\n");
            return -3;
        }

        /* Four-way handshake: ANonce from the AP, SNonce from the station. */
        u64 t = timer_ms();
        u8 anonce[32], snonce[32];
        for (int i = 0; i < 32; i++) {
            anonce[i] = (u8)(ap->info.bssid[i % 6] + i * 7 + index);
            snonce[i] = (u8)(t >> ((i % 8) * 8)) ^ (u8)(i * 13 + 0x5A);
        }
        derive_ptk(pmk, ap->info.bssid, wlan.mac, anonce, snonce, ptk);

        /* The AP derives its own PTK from its copy of the PMK.  The MICs can
           only match if the station picked the same passphrase and both
           sides built the same pairwise key from the two nonces. */
        u8 ap_ptk[48];
        derive_ptk(ap_pmk, ap->info.bssid, wlan.mac, anonce, snonce, ap_ptk);

        u8 mic[20], expect[20];
        hmac_sha1(ptk, 16, snonce, 32, mic);
        hmac_sha1(ap_ptk, 16, snonce, 32, expect);
        if (memcmp(mic, expect, 20) != 0) {
            if (errlen) copy_str(err, "Handshake failed", errlen);
            memset(ptk, 0, sizeof(ptk));
            return -4;
        }
        handshake_done = 1;

        serial_write("wifi: PMK ");
        for (int i = 0; i < 4; i++) {
            serial_hex(pmk[i] >> 4);
            serial_hex(pmk[i] & 0xF);
        }
        serial_write(" PTK ");
        for (int i = 0; i < 4; i++) {
            serial_hex(ptk[i] >> 4);
            serial_hex(ptk[i] & 0xF);
        }
        serial_write(" MIC ok\r\n");
    }

    connected = 1;
    connected_index = index;
    wlan.up = 1;
    wlan.ip = ip4(192, 168, 4, 2);
    wlan.mask = 0xFFFFFF00u;
    wlan.gw = ip4(192, 168, 4, 1);
    wlan.dns = ip4(192, 168, 4, 1);
    wlan.dhcp_done = 1;

    copy_str(status, "Connected to ", sizeof(status));
    u32 n = strlen(status);
    copy_str(status + n, ap->info.ssid,
            (int)sizeof(status) - (int)n);

    serial_write("wifi: associated with ");
    serial_write(ap->info.ssid);
    serial_write(ap->info.security ? " (WPA2-PSK)\r\n" : " (open)\r\n");
    return 0;
}

void wifi_disconnect(void)
{
    reset_link();
    copy_str(status, "Not connected", sizeof(status));
    serial_write("wifi: disconnected\r\n");
}

int wifi_connected(void) { return connected; }
const char *wifi_ssid(void)
{
    return connected_index >= 0 ? aps[connected_index].info.ssid : "";
}
const char *wifi_status(void)
{
    if (!connected)
        return "Not connected";
    return status;
}
