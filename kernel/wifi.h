#ifndef RVOS_WIFI_H
#define RVOS_WIFI_H

#include "types.h"

#define WIFI_MAX_APS 6

typedef struct {
    char ssid[33];
    u8 bssid[6];
    int channel;
    int rssi;
    int security;        /* 0 = open, 1 = WPA2-PSK */
} WifiApInfo;

void wifi_init(void);
void wifi_poll(void);

int  wifi_scan(void);
int  wifi_ap_count(void);
const WifiApInfo *wifi_ap(int index);

int  wifi_connect(int index, const char *passphrase, char *err, int errlen);
void wifi_disconnect(void);
int  wifi_connected(void);
const char *wifi_ssid(void);
const char *wifi_status(void);
int  wifi_security(int index);

#endif
