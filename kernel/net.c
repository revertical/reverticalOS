/* ReverticalOS network stack.
 *
 * Ethernet, ARP, IPv4, ICMP, UDP (DHCP client and DNS resolver), a small TCP
 * client/server and an HTTP/1.0 client.  There is no socket layer: the desktop
 * calls net_http_url() directly and the stack runs in polling mode from
 * net_poll(), which the UI loop calls once per frame.
 *
 * Two interfaces can exist at once: the Intel NIC ("eth0") and the simulated
 * wireless link ("wlan0") implemented in kernel/wifi.c.
 */

#include "net.h"
#include "e1000.h"
#include "wifi.h"
#include "string.h"
#include "timer.h"
#include "serial.h"

#define ETH_TYPE_IP  0x0800
#define ETH_TYPE_ARP 0x0806
#define IP_ICMP 1
#define IP_UDP  17
#define IP_TCP  6

#define ARP_CACHE 8
#define DNS_CACHE 8
#define TCP_CONNS 8
#define TCP_RXMAX 8192

static NetIf *ifs[NET_MAX_IFS];
static int if_count;

static u16 ip_id;

/* ------------------------------------------------------------------------- */
/*  Byte order helpers (wire is big endian)                                   */
/* ------------------------------------------------------------------------- */
static u16 rd16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static u32 rd32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | (u32)p[3];
}
static void wr16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);  p[3] = (u8)v;
}

static u32 ip_parse(const char *s)
{
    u32 parts[4] = { 0, 0, 0, 0 };
    int n = 0;
    while (*s && n < 4) {
        if (*s >= '0' && *s <= '9') {
            parts[n] = parts[n] * 10u + (u32)(*s - '0');
            if (parts[n] > 255) return 0;
        } else if (*s == '.') {
            n++;
        } else {
            return 0;
        }
        s++;
    }
    if (n != 3) return 0;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

const char *net_ip_text(u32 ip)
{
    static char buf[16];
    int k = 0;
    for (int i = 3; i >= 0; i--) {
        u32 part = (ip >> (i * 8)) & 0xFFu;
        char tmp[4];
        int n = 0;
        if (!part) tmp[n++] = '0';
        while (part) { tmp[n++] = (char)('0' + part % 10); part /= 10; }
        while (n) buf[k++] = tmp[--n];
        if (i) buf[k++] = '.';
    }
    buf[k] = 0;
    return buf;
}

static u16 checksum(const u8 *data, u32 len, u32 sum)
{
    while (len > 1) {
        sum += (u32)rd16(data);
        data += 2;
        len -= 2;
    }
    if (len)
        sum += (u32)((u16)data[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (u16)~sum;
}

/* ------------------------------------------------------------------------- */
/*  Interfaces                                                                */
/* ------------------------------------------------------------------------- */
void net_register(NetIf *ifp)
{
    if (if_count < NET_MAX_IFS)
        ifs[if_count++] = ifp;
}

int net_if_count(void) { return if_count; }
NetIf *net_if_at(int index)
{
    return (index >= 0 && index < if_count) ? ifs[index] : 0;
}
NetIf *net_if_by_name(const char *name)
{
    for (int i = 0; i < if_count; i++)
        if (strcmp(ifs[i]->name, name) == 0)
            return ifs[i];
    return 0;
}
NetIf *net_wired(void) { return net_if_by_name("eth0"); }
NetIf *net_wifi(void) { return net_if_by_name("wlan0"); }

int net_ready(void) { return if_count > 0; }
int net_link_up(void)
{
    for (int i = 0; i < if_count; i++)
        if (ifs[i]->up) return 1;
    return 0;
}

static int mac_equal(const u8 *a, const u8 *b)
{
    return memcmp(a, b, 6) == 0;
}

/* ------------------------------------------------------------------------- */
/*  ARP                                                                       */
/* ------------------------------------------------------------------------- */
typedef struct {
    u32 ip;
    u8 mac[6];
    u8 valid;
} ArpEntry;

static ArpEntry arp_cache[ARP_CACHE];

static ArpEntry *arp_lookup(u32 ip)
{
    for (int i = 0; i < ARP_CACHE; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip)
            return &arp_cache[i];
    return 0;
}

static void arp_store(u32 ip, const u8 *mac)
{
    int slot = -1;
    for (int i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) { slot = i; break; }
        if (!arp_cache[i].valid && slot < 0) slot = i;
    }
    if (slot < 0) slot = 0;
    arp_cache[slot].ip = ip;
    memcpy(arp_cache[slot].mac, mac, 6);
    arp_cache[slot].valid = 1;
}

static void arp_send(NetIf *ifp, u16 op, const u8 *dst_mac, u32 dst_ip)
{
    u8 frame[42];
    memset(frame, 0xFF, 6);
    if (dst_mac) memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, ifp->mac, 6);
    wr16(frame + 12, ETH_TYPE_ARP);
    wr16(frame + 14, 1);                 /* ethernet */
    wr16(frame + 16, ETH_TYPE_IP);
    frame[18] = 6; frame[19] = 4;
    wr16(frame + 20, op);
    memcpy(frame + 22, ifp->mac, 6);
    wr32(frame + 28, ifp->ip);
    if (dst_mac) memcpy(frame + 32, dst_mac, 6);
    else memset(frame + 32, 0, 6);
    wr32(frame + 38, dst_ip);
    ifp->send(ifp, frame, sizeof(frame));
}

static void arp_input(NetIf *ifp, const u8 *p, u32 len)
{
    if (len < 28) return;
    u16 op = rd16(p + 6);
    const u8 *sha = p + 8;
    u32 spa = rd32(p + 14);
    u32 tpa = rd32(p + 24);

    arp_store(spa, sha);
    if (op == 1 && tpa == ifp->ip)
        arp_send(ifp, 2, sha, spa);
}

static int arp_resolve(NetIf *ifp, u32 ip, u8 mac[6], int timeout_ms)
{
    ArpEntry *e = arp_lookup(ip);
    if (e) { memcpy(mac, e->mac, 6); return 0; }

    u64 start = timer_ms();
    while (timer_ms() - start < (u64)timeout_ms) {
        arp_send(ifp, 1, 0, ip);
        u64 round = timer_ms();
        while (timer_ms() - round < 120) {
            net_poll();
            e = arp_lookup(ip);
            if (e) { memcpy(mac, e->mac, 6); return 0; }
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/*  IPv4                                                                      */
/* ------------------------------------------------------------------------- */
static int ip_send_from(NetIf *ifp, u32 src, u32 dst, u8 proto,
                        const void *payload, u32 len)
{
    u8 frame[1600];
    if (len > 1500) return -1;
    if (len + 28 > sizeof(frame)) return -1;

    u8 dstmac[6];
    int direct = 0;
    if (dst == 0xFFFFFFFFu) {
        memset(dstmac, 0xFF, 6);
        direct = 1;
    }
    if (!direct) {
        u32 target = dst;
        if (ifp->mask && (dst & ifp->mask) != (src & ifp->mask) && ifp->gw)
            target = ifp->gw;
        if (arp_resolve(ifp, target, dstmac, 600) < 0)
            return -1;
    }

    memcpy(frame, dstmac, 6);
    memcpy(frame + 6, ifp->mac, 6);
    wr16(frame + 12, ETH_TYPE_IP);

    u8 *ip = frame + 14;
    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, (u16)(20 + len));
    wr16(ip + 4, ++ip_id);
    wr16(ip + 6, 0x4000);                /* don't fragment */
    ip[8] = 64;
    ip[9] = proto;
    wr16(ip + 10, 0);
    wr32(ip + 12, src);
    wr32(ip + 16, dst);
    wr16(ip + 10, checksum(ip, 20, 0));
    memcpy(ip + 20, payload, len);
    return ifp->send(ifp, frame, 14 + 20 + len) < 0 ? -1 : 0;
}

static int ip_send(NetIf *ifp, u32 dst, u8 proto, const void *payload, u32 len)
{
    return ip_send_from(ifp, ifp->ip, dst, proto, payload, len);
}

/* ------------------------------------------------------------------------- */
/*  ICMP                                                                      */
/* ------------------------------------------------------------------------- */
static u16 ping_id;
static u16 ping_seq;
static volatile int ping_reply;

static void icmp_input(NetIf *ifp, const u8 *p, u32 len, u32 src)
{
    if (len < 8) return;
    if (p[0] != 8) {                     /* echo request only */
        if (p[0] == 0) ping_reply = 1;
        return;
    }
    u8 reply[1600];
    if (len > sizeof(reply)) len = sizeof(reply);
    memcpy(reply, p, len);
    reply[0] = 0;
    reply[2] = reply[3] = 0;
    wr16(reply + 2, checksum(reply, len, 0));
    ip_send(ifp, src, IP_ICMP, reply, len);
}

int net_ping(NetIf *ifp, u32 host, int timeout_ms)
{
    if (!ifp || !ifp->up) return -1;
    u8 packet[32];
    memset(packet, 0, sizeof(packet));
    packet[0] = 8;
    ping_id++;
    ping_seq++;
    wr16(packet + 4, ping_id);
    wr16(packet + 6, ping_seq);
    for (int i = 8; i < 32; i++) packet[i] = (u8)(i * 7 + 1);
    wr16(packet + 2, checksum(packet, sizeof(packet), 0));

    ping_reply = 0;
    if (ip_send(ifp, host, IP_ICMP, packet, sizeof(packet)) < 0)
        return -1;
    u64 start = timer_ms();
    while (timer_ms() - start < (u64)timeout_ms) {
        net_poll();
        if (ping_reply)
            return (int)(timer_ms() - start);
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/*  UDP, DHCP, DNS                                                            */
/* ------------------------------------------------------------------------- */
static int udp_send(NetIf *ifp, u32 src, u32 dst, u16 sport, u16 dport,
                    const void *data, u32 len)
{
    u8 packet[1500];
    if (len + 8 > sizeof(packet)) return -1;
    wr16(packet, sport);
    wr16(packet + 2, dport);
    wr16(packet + 4, (u16)(8 + len));
    wr16(packet + 6, 0);
    memcpy(packet + 8, data, len);

    u32 sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += IP_UDP;
    sum += (u32)(8 + len);
    u16 c = checksum(packet, 8 + len, sum);
    if (c == 0) c = 0xFFFF;
    wr16(packet + 6, c);

    return ip_send_from(ifp, src, dst, IP_UDP, packet, 8 + len);
}

/* ---- DHCP ---------------------------------------------------------------- */
static u32 dhcp_xid;
static volatile int dhcp_got_offer;
static volatile int dhcp_got_ack;
static u32 dhcp_offer_ip;
static u32 dhcp_server_ip;
static u32 dhcp_mask;
static u32 dhcp_router;
static u32 dhcp_dns;

static u8 *dhcp_put_option(u8 *p, u8 code, u8 len, const void *value)
{
    *p++ = code;
    *p++ = len;
    if (value) memcpy(p, value, len);
    p += len;
    return p;
}

static u32 dhcp_build(NetIf *ifp, u8 type, u32 requested, u8 *out)
{
    memset(out, 0, 300);
    out[0] = 1;                          /* BOOTREQUEST */
    out[1] = 1;                          /* ethernet */
    out[2] = 6;
    wr32(out + 4, dhcp_xid);
    wr16(out + 8, 0x8000);               /* broadcast flag */
    memcpy(out + 28, ifp->mac, 6);

    u8 *p = out + 236;
    wr32(p, 0x63825363u);
    p += 4;
    p = dhcp_put_option(p, 53, 1, &type);
    u8 params[] = { 1, 3, 6, 15, 28, 51, 54 };
    p = dhcp_put_option(p, 55, sizeof(params), params);
    u8 be[4];
    if (requested) {
        wr32(be, requested);
        p = dhcp_put_option(p, 50, 4, be);
    }
    if (dhcp_server_ip) {
        wr32(be, dhcp_server_ip);
        p = dhcp_put_option(p, 54, 4, be);
    }
    static const char hostname[] = "revertical";
    p = dhcp_put_option(p, 12, sizeof(hostname) - 1, hostname);
    *p++ = 255;
    return (u32)(p - out);
}

static void dhcp_parse(NetIf *ifp, const u8 *msg, u32 len)
{
    if (len < 240) return;
    if (rd32(msg + 236) != 0x63825363u) return;
    if (rd32(msg + 4) != dhcp_xid) return;

    u8 type = 0;
    const u8 *p = msg + 240;
    const u8 *end = msg + len;
    while (p + 1 < end && *p != 255) {
        u8 code = *p++;
        if (code == 0) continue;
        if (p >= end) break;
        u8 olen = *p++;
        if (p + olen > end) break;
        if (code == 53 && olen >= 1) type = p[0];
        else if (code == 54 && olen >= 4) dhcp_server_ip = rd32(p);
        else if (code == 1 && olen >= 4) dhcp_mask = rd32(p);
        else if (code == 3 && olen >= 4) dhcp_router = rd32(p);
        else if (code == 6 && olen >= 4) dhcp_dns = rd32(p);
        p += olen;
    }

    if (type == 2) {
        dhcp_offer_ip = rd32(msg + 16);
        dhcp_got_offer = 1;
    } else if (type == 5) {
        dhcp_offer_ip = rd32(msg + 16);
        if (!dhcp_mask) dhcp_mask = 0xFFFFFF00u;
        if (!dhcp_router) dhcp_router = ifp->gw;
        if (!dhcp_dns) dhcp_dns = dhcp_router;
        ifp->ip = dhcp_offer_ip;
        ifp->mask = dhcp_mask;
        ifp->gw = dhcp_router;
        ifp->dns = dhcp_dns;
        ifp->dhcp_done = 1;
        dhcp_got_ack = 1;
    }
}

int net_dhcp(NetIf *ifp, u32 timeout_ms)
{
    if (!ifp || !ifp->up) return -1;
    u8 msg[300];
    u32 old_ip = ifp->ip;

    dhcp_xid = (u32)(timer_ms() * 2654435761u) ^ 0x52564F53u;
    dhcp_got_offer = 0;
    dhcp_got_ack = 0;
    dhcp_server_ip = 0;
    dhcp_mask = dhcp_router = dhcp_dns = 0;
    ifp->ip = 0;

    u64 start = timer_ms();
    int sent = 0;
    while (timer_ms() - start < (u64)timeout_ms && !dhcp_got_offer) {
        if (sent < 3) {
            u32 len = dhcp_build(ifp, 1, 0, msg);
            udp_send(ifp, 0, 0xFFFFFFFFu, 68, 67, msg, len);
            sent++;
        }
        u64 round = timer_ms();
        while (timer_ms() - round < 400 && !dhcp_got_offer)
            net_poll();
    }

    if (!dhcp_got_offer) {
        ifp->ip = old_ip;
        return -1;
    }

    dhcp_got_ack = 0;
    start = timer_ms();
    sent = 0;
    while (timer_ms() - start < (u64)timeout_ms && !dhcp_got_ack) {
        if (sent < 3) {
            u32 len = dhcp_build(ifp, 3, dhcp_offer_ip, msg);
            udp_send(ifp, 0, 0xFFFFFFFFu, 68, 67, msg, len);
            sent++;
        }
        u64 round = timer_ms();
        while (timer_ms() - round < 400 && !dhcp_got_ack)
            net_poll();
    }

    if (!dhcp_got_ack) {
        ifp->ip = old_ip;
        return -1;
    }

    serial_write("dhcp: ");
    serial_write(net_ip_text(ifp->ip));
    serial_write(" gw ");
    serial_write(net_ip_text(ifp->gw));
    serial_write(" dns ");
    serial_write(net_ip_text(ifp->dns));
    serial_write("\r\n");
    return 0;
}

/* ---- DNS ----------------------------------------------------------------- */
typedef struct {
    u32 ip;
    char name[40];
    u8 valid;
} DnsEntry;

static DnsEntry dns_cache[DNS_CACHE];
static u16 dns_id;
static volatile int dns_done;
static u32 dns_answer;

static int dns_name_equal(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

u32 net_resolve(NetIf *ifp, const char *host, int timeout_ms)
{
    u32 direct = ip_parse(host);
    if (direct) return direct;

    for (int i = 0; i < DNS_CACHE; i++)
        if (dns_cache[i].valid && dns_name_equal(dns_cache[i].name, host))
            return dns_cache[i].ip;

    if (!ifp || !ifp->dns) return 0;

    u8 query[256];
    memset(query, 0, sizeof(query));
    dns_id++;
    wr16(query, dns_id);
    wr16(query + 2, 0x0100);             /* recursion desired */
    wr16(query + 4, 1);
    u32 k = 12;
    const char *p = host;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        u32 n = (u32)(dot - p);
        if (n == 0 || n > 63 || k + n + 2 >= sizeof(query)) return 0;
        query[k++] = (u8)n;
        memcpy(query + k, p, n);
        k += n;
        if (!*dot) break;
        p = dot + 1;
    }
    query[k++] = 0;
    wr16(query + k, 1);                  /* A */
    wr16(query + k + 2, 1);              /* IN */
    k += 4;

    dns_done = 0;
    dns_answer = 0;
    udp_send(ifp, ifp->ip, ifp->dns, 49000 + (dns_id & 0x3FF), 53, query, k);

    u64 start = timer_ms();
    while (timer_ms() - start < (u64)timeout_ms && !dns_done)
        net_poll();
    if (!dns_done) return 0;

    int slot = -1;
    for (int i = 0; i < DNS_CACHE; i++) {
        if (dns_cache[i].valid && dns_name_equal(dns_cache[i].name, host)) {
            slot = i;
            break;
        }
        if (!dns_cache[i].valid) { slot = i; break; }
    }
    if (slot < 0) slot = (int)(dns_id % DNS_CACHE);
    dns_cache[slot].valid = 1;
    dns_cache[slot].ip = dns_answer;
    u32 n = 0;
    while (host[n] && n < 39) { dns_cache[slot].name[n] = host[n]; n++; }
    dns_cache[slot].name[n] = 0;
    return dns_answer;
}

static void dns_input(const u8 *p, u32 len)
{
    if (len < 12 || rd16(p) != dns_id) return;
    u16 answers = rd16(p + 6);
    u32 off = 12;
    while (off < len && p[off]) {
        if ((p[off] & 0xC0) == 0xC0) { off += 2; break; }
        off += 1u + p[off];
    }
    if (off + 4 > len) return;
    off += 4;                            /* skip QTYPE/QCLASS */
    for (u16 i = 0; i < answers && off + 10 <= len; i++) {
        if ((p[off] & 0xC0) == 0xC0) off += 2;
        else {
            while (off < len && p[off]) off += 1u + p[off];
            off++;
        }
        if (off + 10 > len) return;
        u16 type = rd16(p + off);
        u16 rdlen = rd16(p + off + 8);
        off += 10;
        if (type == 1 && rdlen == 4 && off + 4 <= len) {
            dns_answer = rd32(p + off);
            dns_done = 1;
            return;
        }
        off += rdlen;
    }
}

/* ------------------------------------------------------------------------- */
/*  TCP                                                                       */
/* ------------------------------------------------------------------------- */
enum { TS_CLOSED, TS_SYN_SENT, TS_SYN_RCVD, TS_ESTAB, TS_CLOSE_WAIT, TS_FIN_WAIT };

typedef struct {
    int used;
    int server;
    NetIf *ifp;
    u32 rip;
    u16 lport, rport;
    u32 snd_nxt, rcv_nxt;
    u8 state;
    int got_fin;
    int responded;
    u8 rx[TCP_RXMAX];
    int rxlen;
} TcpConn;

static TcpConn conns[TCP_CONNS];
static u32 tcp_clock;

static TcpConn *tcp_find(NetIf *ifp, u16 lport, u32 rip, u16 rport)
{
    for (int i = 0; i < TCP_CONNS; i++) {
        TcpConn *c = &conns[i];
        if (c->used && c->ifp == ifp && c->lport == lport &&
            c->rip == rip && c->rport == rport)
            return c;
    }
    return 0;
}

static TcpConn *tcp_alloc(void)
{
    for (int i = 0; i < TCP_CONNS; i++)
        if (!conns[i].used) {
            memset(&conns[i], 0, sizeof(conns[i]));
            conns[i].used = 1;
            return &conns[i];
        }
    return 0;
}

static void tcp_send(NetIf *ifp, u32 dst, u16 sport, u16 dport,
                     u32 seq, u32 ack, u8 flags, const void *data, u32 len)
{
    u8 packet[1500];
    if (len + 20 > sizeof(packet)) return;
    wr16(packet, sport);
    wr16(packet + 2, dport);
    wr32(packet + 4, seq);
    wr32(packet + 8, ack);
    packet[12] = 0x50;
    packet[13] = flags;
    wr16(packet + 14, 8192);
    wr16(packet + 16, 0);
    wr16(packet + 18, 0);
    if (len) memcpy(packet + 20, data, len);

    u32 sum = 0;
    sum += (ifp->ip >> 16) & 0xFFFF; sum += ifp->ip & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += IP_TCP;
    sum += (u32)(20 + len);
    wr16(packet + 16, checksum(packet, 20 + len, sum));

    ip_send(ifp, dst, IP_TCP, packet, 20 + len);
}

static int tcp_send_data(TcpConn *c, const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    while (len) {
        u32 chunk = len > 1000 ? 1000 : len;
        tcp_send(c->ifp, c->rip, c->lport, c->rport, c->snd_nxt,
                 c->rcv_nxt, 0x18, p, chunk);
        c->snd_nxt += chunk;
        p += chunk;
        len -= chunk;
    }
    return 0;
}

/* Built-in site served to HTTP clients (used by the simulated wireless AP). */
int net_wifi_site(const char *path, char *out, int max)
{
    const char *body;
    if (path[0] == '/' && path[1] == 'a') {
        body =
            "<html><head><title>ReverticalOS applications</title></head>"
            "<body><h1>Applications</h1>"
            "<p>Executables use the <b>RVEX</b> container and libraries use "
            "<b>RVLB</b>. Both are loaded by the kernel module loader.</p>"
            "<ul><li>Browser.rvx</li><li>Hello.rvx</li>"
            "<li>System Info.rvx</li><li>rvstd.lib</li></ul>"
            "<p><a href=\"/\">Back to the index</a></p></body></html>";
    } else if (path[0] == '/' && path[1] == 'n') {
        body =
            "<html><head><title>Network</title></head><body>"
            "<h1>RevertiaNet</h1>"
            "<p>This page is served by the in-kernel HTTP server over the "
            "simulated 802.11 link. The station associated with WPA2-PSK, "
            "derived the PTK and completed the four-way handshake.</p>"
            "<p><a href=\"/\">Home</a> "
            "<a href=\"/apps\">Applications</a></p></body></html>";
    } else {
        body =
            "<html><head><title>RevertiaNet</title></head>"
            "<body><h1>Connected to RevertiaNet</h1>"
            "<p>You reached this page over the virtual wireless adapter, "
            "using the ReverticalOS TCP/IP stack and its HTTP client.</p>"
            "<p>Wi-Fi: WPA2-PSK, PBKDF2-HMAC-SHA1 PMK, four-way handshake "
            "with HMAC-SHA1 MIC.</p>"
            "<p><a href=\"/apps\">Applications</a> "
            "<a href=\"/net\">How this works</a></p>"
            "<p><a href=\"/\">Reload</a></p></body></html>";
    }
    int n = 0;
    while (body[n] && n < max - 1) { out[n] = body[n]; n++; }
    out[n] = 0;
    return n;
}

static void http_serve(TcpConn *c)
{
    char path[64];
    path[0] = '/'; path[1] = 0;
    if (c->rxlen > 4 && memcmp(c->rx, "GET ", 4) == 0) {
        int k = 0;
        for (int i = 4; i < c->rxlen && c->rx[i] != ' ' && k < 60; i++)
            path[k++] = c->rx[i];
        if (k == 0) { path[0] = '/'; k = 1; }
        path[k] = 0;
    }

    static char body[2048];
    net_wifi_site(path, body, sizeof(body) - 1);

    char header[160];
    int hn = 0;
    const char *h = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                    "Connection: close\r\n\r\n";
    while (h[hn]) { header[hn] = h[hn]; hn++; }
    header[hn] = 0;

    tcp_send_data(c, header, (u32)hn);
    tcp_send_data(c, body, (u32)strlen(body));
    tcp_send(c->ifp, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt,
             0x11, 0, 0);                  /* FIN|ACK */
    c->snd_nxt++;
    c->state = TS_FIN_WAIT;
    c->responded = 1;
}

static void tcp_input(NetIf *ifp, const u8 *ip, const u8 *tcp, u32 len)
{
    if (len < 20) return;
    u16 sport = rd16(tcp);
    u16 dport = rd16(tcp + 2);
    u32 seq = rd32(tcp + 4);
    u8 off_flags = tcp[12];
    u8 flags = tcp[13];
    u32 hdr = (u32)(off_flags >> 4) * 4;
    if (hdr < 20 || hdr > len) return;
    u32 payload = len - hdr;

    u32 rip = rd32(ip + 12);

    TcpConn *c = tcp_find(ifp, dport, rip, sport);
    if (!c) {
        if ((flags & 0x02) && dport == 80 && !(flags & 0x10)) {
            c = tcp_alloc();
            if (!c) return;
            c->server = 1;
            c->ifp = ifp;
            c->rip = rip;
            c->lport = 80;
            c->rport = sport;
            c->rcv_nxt = seq + 1;
            c->snd_nxt = (++tcp_clock * 7919u) ^ 0x52564F53u;
            tcp_send(ifp, rip, 80, sport, c->snd_nxt, c->rcv_nxt,
                     0x12, 0, 0);          /* SYN|ACK */
            c->snd_nxt++;
            c->state = TS_SYN_RCVD;
        }
        return;
    }

    if (flags & 0x04) return;              /* RST */

    /* The built-in server is write-once: after FIN, the client's ACK (or
       FIN|ACK) releases the slot for the next request. */
    if (c->server && c->state == TS_FIN_WAIT && (flags & 0x10)) {
        c->used = 0;
        return;
    }

    if ((flags & 0x02) && (flags & 0x10)) {
        if (c->state == TS_SYN_SENT) {
            c->rcv_nxt = seq + 1;
            c->snd_nxt = rd32(tcp + 8);
            c->state = TS_ESTAB;
            tcp_send(ifp, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt,
                     0x10, 0, 0);
        }
        return;
    }

    if (flags & 0x10) {
        if (c->state == TS_SYN_RCVD && !c->server)
            c->state = TS_ESTAB;
        if (c->server && c->state == TS_SYN_RCVD)
            c->state = TS_ESTAB;
    }

    if (payload) {
        if (seq == c->rcv_nxt) {
            u32 space = TCP_RXMAX - (u32)c->rxlen;
            u32 copy = payload < space ? payload : space;
            if (copy) {
                memcpy(c->rx + c->rxlen, tcp + hdr, copy);
                c->rxlen += (int)copy;
                c->rcv_nxt += payload;
            }
            tcp_send(ifp, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt,
                     0x10, 0, 0);
            if (c->server && !c->responded && c->rxlen > 4)
                http_serve(c);
        }
    }

    if (flags & 0x01) {
        c->rcv_nxt = seq + payload + 1;
        c->got_fin = 1;
        tcp_send(ifp, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt,
                 0x10, 0, 0);
        if (!c->server) c->state = TS_CLOSE_WAIT;
    }
}

/* Split the raw IPv4 packet into UDP/TCP dispatch. */
static void ip_input(NetIf *ifp, const u8 *p, u32 len)
{
    if (len < 20) return;
    if ((p[0] >> 4) != 4) return;
    u32 ihl = (u32)(p[0] & 0x0F) * 4;
    if (ihl < 20 || ihl > len) return;
    u16 total = rd16(p + 2);
    if (total < ihl || total > len) total = (u16)len;
    u8 proto = p[9];
    u32 src = rd32(p + 12);
    u32 dst = rd32(p + 16);

    u32 bcast = ifp->ip | ~ifp->mask;
    if (dst != ifp->ip && dst != 0xFFFFFFFFu && dst != bcast)
        return;

    const u8 *payload = p + ihl;
    u32 plen = total - ihl;

    if (proto == IP_ICMP) {
        icmp_input(ifp, payload, plen, src);
    } else if (proto == IP_UDP) {
        if (plen < 8) return;
        u16 dport = rd16(payload + 2);
        u32 ulen = rd16(payload + 4);
        if (ulen < 8 || ulen > plen) ulen = plen;
        const u8 *data = payload + 8;
        u32 dlen = ulen - 8;
        if (dport == 68) dhcp_parse(ifp, data, dlen);
        else if (rd16(payload + 2) == 49000 + (dns_id & 0x3FF))
            dns_input(data, dlen);
    } else if (proto == IP_TCP) {
        tcp_input(ifp, p, payload, plen);
    }
}

static void eth_input(NetIf *ifp, const u8 *frame, u32 len)
{
    if (len < 14) return;
    u16 type = rd16(frame + 12);
    const u8 *p = frame + 14;
    u32 plen = len - 14;
    if (!mac_equal(frame, ifp->mac) && !mac_equal(frame, (const u8 *)"\xFF\xFF\xFF\xFF\xFF\xFF"))
        return;
    if (type == ETH_TYPE_ARP) arp_input(ifp, p, plen);
    else if (type == ETH_TYPE_IP) ip_input(ifp, p, plen);
}

void netif_input(NetIf *ifp, const void *frame, u32 len)
{
    if (!ifp || !ifp->up) return;
    ifp->rx_frames++;
    eth_input(ifp, (const u8 *)frame, len);
}

void net_poll(void)
{
    e1000_poll();
    wifi_poll();
}

/* ------------------------------------------------------------------------- */
/*  HTTP client                                                               */
/* ------------------------------------------------------------------------- */
static TcpConn *tcp_connect(NetIf *ifp, u32 dst, u16 port, int timeout_ms)
{
    TcpConn *c = tcp_alloc();
    if (!c) return 0;
    c->ifp = ifp;
    c->rip = dst;
    c->lport = (u16)(49152 + ((++tcp_clock * 37u) & 0x3FFF));
    c->rport = port;
    c->snd_nxt = (++tcp_clock * 2654435761u);
    c->rcv_nxt = 0;
    c->state = TS_SYN_SENT;
    tcp_send(ifp, dst, c->lport, port, c->snd_nxt, 0, 0x02, 0, 0);
    c->snd_nxt++;

    u64 start = timer_ms();
    while (timer_ms() - start < (u64)timeout_ms && c->state != TS_ESTAB) {
        net_poll();
        if (c->got_fin) break;
    }
    if (c->state != TS_ESTAB) {
        c->used = 0;
        return 0;
    }
    return c;
}

static void tcp_close(TcpConn *c)
{
    if (!c) return;
    if (c->state == TS_ESTAB || c->state == TS_CLOSE_WAIT) {
        tcp_send(c->ifp, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt,
                 0x11, 0, 0);
        c->snd_nxt++;
    }
    c->used = 0;
}

static int http_strip(char *buf, int len, int *status_out)
{
    int i;
    *status_out = 0;
    if (len > 12 && memcmp(buf, "HTTP/", 5) == 0) {
        int code = 0;
        for (int k = 9; k < len && buf[k] >= '0' && buf[k] <= '9'; k++)
            code = code * 10 + (buf[k] - '0');
        *status_out = code;
    }
    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            int start = i + 4;
            int n = len - start;
            memmove(buf, buf + start, (size_t)n);
            buf[n] = 0;
            return n;
        }
    }
    buf[0] = 0;
    return 0;
}

int net_http_get(NetIf *ifp, u32 host, const char *hostname,
                 const char *path, char *out, int max, int timeout_ms)
{
    TcpConn *c = tcp_connect(ifp, host, 80, timeout_ms);
    if (!c) return -1;

    char request[256];
    int n = 0;
    const char *a = "GET ";
    while (*a && n < 250) request[n++] = *a++;
    const char *p = path;
    while (*p && n < 250) request[n++] = *p++;
    a = " HTTP/1.0\r\nHost: ";
    while (*a && n < 250) request[n++] = *a++;
    p = hostname;
    while (*p && n < 250) request[n++] = *p++;
    a = "\r\nUser-Agent: ReverticalOS/1.0\r\nAccept: */*\r\n"
        "Connection: close\r\n\r\n";
    while (*a && n < 255) request[n++] = *a++;

    tcp_send_data(c, request, (u32)n);

    int got = 0;
    u64 start = timer_ms();
    while (timer_ms() - start < (u64)timeout_ms) {
        net_poll();
        int fresh = c->rxlen - got;
        if (fresh > 0) {
            int room = max - got - 1;
            if (fresh > room) fresh = room;
            if (fresh > 0) {
                memcpy(out + got, c->rx + got, (size_t)fresh);
                got += fresh;
            }
            if (got >= max - 1) break;
        }
        if (c->got_fin) break;
        if (c->state == TS_CLOSED) break;
    }

    tcp_close(c);
    out[got] = 0;
    if (got == 0) return -1;

    int status = 0;
    int body = http_strip(out, got, &status);
    if (status == 0) return -2;
    if (status != 200)
        return -status;              /* -3xx/-4xx/-5xx: HTTP error */
    return body;
}

static int host_is_local(const char *host)
{
    const char *suffix = ".local";
    int n = (int)strlen(host);
    int m = 6;
    if (strcmp(host, "localhost") == 0 || strcmp(host, "revertical") == 0)
        return 1;
    if (n < m) return 0;
    for (int i = 0; i < m; i++) {
        char a = host[n - m + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

int net_http_url(const char *url, char *out, int max, char *err, int errlen,
                 int timeout_ms)
{
    if (errlen > 0) err[0] = 0;

    const char *p = url;
    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p' &&
        p[4] == ':' && p[5] == '/' && p[6] == '/')
        p += 7;
    else {
        const char *e = "Only http:// URLs are supported";
        int i = 0;
        while (e[i] && i < errlen - 1) { err[i] = e[i]; i++; }
        if (errlen) err[i] = 0;
        return -1;
    }

    char host[64];
    char path[96];
    int hn = 0;
    while (*p && *p != '/' && *p != ':' && hn < 60) host[hn++] = *p++;
    host[hn] = 0;
    if (!hn) {
        const char *e = "Missing host name";
        int i = 0;
        while (e[i] && i < errlen - 1) { err[i] = e[i]; i++; }
        if (errlen) err[i] = 0;
        return -1;
    }
    int port = 80;
    if (*p == ':') {
        p++;
        port = 0;
        while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); p++; }
    }
    int pn = 0;
    if (*p != '/') path[pn++] = '/';
    while (*p && pn < 90) path[pn++] = *p++;
    path[pn] = 0;

    NetIf *ifp = 0;
    u32 ip = 0;
    if (host_is_local(host)) {
        if (!net_wifi() || !net_wifi()->up) {
            const char *e = "Connect to Wi-Fi in Settings first";
            int i = 0;
            while (e[i] && i < errlen - 1) { err[i] = e[i]; i++; }
            if (errlen) err[i] = 0;
            return -1;
        }
        ifp = net_wifi();
        ip = 0xC0A80401u;                /* 192.168.4.1: the virtual AP */
    } else {
        ifp = net_wired();
        if (!ifp || !ifp->up) ifp = net_wifi();
        if (!ifp || !ifp->up) {
            const char *e = "No network link";
            int i = 0;
            while (e[i] && i < errlen - 1) { err[i] = e[i]; i++; }
            if (errlen) err[i] = 0;
            return -1;
        }
        ip = net_resolve(ifp, host, timeout_ms);
    }
    if (!ip) {
        const char *e = "DNS lookup failed";
        int i = 0;
        while (e[i] && i < errlen - 1) { err[i] = e[i]; i++; }
        if (errlen) err[i] = 0;
        return -1;
    }
    if (port != 80) {
        const char *e = "Only port 80 is supported";
        int i = 0;
        while (e[i] && i < errlen - 1) { err[i] = e[i]; i++; }
        if (errlen) err[i] = 0;
        return -1;
    }

    int rc = net_http_get(ifp, ip, host, path, out, max, timeout_ms);
    if (rc < 0 && rc > -300 && errlen > 0) {
        const char *e = rc == -1 ? "Connection failed or timed out"
                                 : "Malformed HTTP response";
        int i = 0;
        while (e[i] && i < errlen - 1) { err[i] = e[i]; i++; }
        err[i] = 0;
    }
    return rc;
}

const char *net_status_line(void)
{
    static char line[96];
    int k = 0;
    for (int i = 0; i < if_count; i++) {
        NetIf *f = ifs[i];
        if (i) { line[k++] = ' '; line[k++] = '|'; line[k++] = ' '; }
        for (int j = 0; f->name[j] && k < 20; j++) line[k++] = f->name[j];
        line[k++] = ' ';
        const char *ip = net_ip_text(f->ip);
        for (int j = 0; ip[j] && k < 90; j++) line[k++] = ip[j];
    }
    if (!if_count) {
        const char *s = "no network adapters";
        while (*s) line[k++] = *s++;
    }
    line[k] = 0;
    return line;
}

int net_pending_packets(void)
{
    int pending = 0;
    for (int i = 0; i < if_count; i++)
        pending += (int)ifs[i]->rx_frames;
    return pending;
}

void net_init(void)
{
    if_count = 0;
    ip_id = 0;
    tcp_clock = 0;
    memset(arp_cache, 0, sizeof(arp_cache));
    memset(dns_cache, 0, sizeof(dns_cache));
    memset(conns, 0, sizeof(conns));

    if (e1000_init() == 0) {
        NetIf *eth = net_wired();
        if (eth && net_dhcp(eth, 1600) != 0)
            serial_write("net: DHCP failed on eth0\r\n");
    } else {
        serial_write("net: no e1000 adapter found\r\n");
    }

    wifi_init();
}
