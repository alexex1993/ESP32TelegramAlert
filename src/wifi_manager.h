#pragma once

#include <stddef.h>

#include "esp_err.h"

// One-time Wi-Fi subsystem bring-up: netif, the default event loop and the
// wifi driver init, plus the STA event handlers. Does not select a mode or
// start the radio. Call exactly once from app_main before connect_sta() or
// start_ap().
void wifi_manager_init(void);

// Brings up the station interface against the SSID/password in settings and
// blocks until an IP is acquired or APP_WIFI_MAX_RETRY attempts have failed.
// Returns ESP_FAIL on exhaustion -- the caller is expected to fall back to the
// provisioning AP rather than spin forever on bad credentials.
esp_err_t wifi_manager_connect_sta(void);

// Brings up an open SoftAP named `ssid` with a DHCP server, for first-boot
// provisioning. The AP runs on the ESP-IDF default 192.168.4.1; the URL to
// show on screen is fetched from wifi_manager_get_ap_ip().
esp_err_t wifi_manager_start_ap(const char *ssid);

// Writes the AP's own IPv4 address (dotted quad) into buf. Call after
// wifi_manager_start_ap().
void wifi_manager_get_ap_ip(char *buf, size_t size);

// What the link looks like right now, for the /status reply.
typedef struct {
    bool connected;      // Associated *and* holding an address.
    char ssid[33];       // Empty when not associated.
    int8_t rssi;         // dBm, as reported by the AP record.
    uint8_t channel;
    char ip[16];         // Dotted quad, empty when there is no lease.
} wifi_manager_info_t;

// Safe to call from any task: the AP record comes from esp_wifi (which is
// thread-safe) and the address from a word the event handler publishes.
void wifi_manager_get_info(wifi_manager_info_t *out);
