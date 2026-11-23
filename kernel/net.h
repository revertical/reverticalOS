#ifndef RVOS_NET_H
#define RVOS_NET_H

#include "types.h"

#define NET_MAX_IFS 4

typedef struct NetIf NetIf;

struct NetIf {
    char name[8];
    u8 mac[6];
    u8 up;
    u32 ip, mask, gw, dns;
    int (*send)(NetIf *ifp, const void *frame, u32 len);
    NetIf *peer;            /* simulated link: frames go straight to peer */
    void *priv;
    u32 tx_frames, rx_frames;
    u8 dhcp_done;
};

void   net_init(void);
void   net_poll(void);
int    net_ready(void);
NetIf *net_if_at(int index);
int    net_if_count(void);
NetIf *net_if_by_name(const char *name);
NetIf *net_wired(void);
NetIf *net_wifi(void);
void   net_register(NetIf *ifp);
void   netif_input(NetIf *ifp, const void *frame, u32 len);

int    net_link_up(void);
int    net_dhcp(NetIf *ifp, u32 timeout_ms);
u32    net_resolve(NetIf *ifp, const char *host, int timeout_ms);
int    net_ping(NetIf *ifp, u32 host, int timeout_ms);
int    net_http_get(NetIf *ifp, u32 host, const char *hostname,
                    const char *path, char *out, int max, int timeout_ms);
int    net_http_url(const char *url, char *out, int max, char *err, int errlen,
                    int timeout_ms);

const char *net_ip_text(u32 ip);
const char *net_status_line(void);
int    net_wifi_site(const char *path, char *out, int max);

int    net_pending_packets(void);

#endif
