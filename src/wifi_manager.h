#pragma once

#include <stddef.h>

#include "esp_err.h"

// One-time Wi-Fi subsystem bring-up: netif, the default event loop and the
// wifi driver init, plus the STA event handlers. Does not select a mode or
// start the radio. Call exactly once from app_main before connect_sta() or
// start_ap().
void wifi_manager_init(void);

// Brings up the station interface and walks the configured networks (up to
// three, in slot order -- see app_settings_t) until one acquires an IP,
// giving each APP_WIFI_MAX_RETRY attempts. Returns ESP_FAIL when every
// configured network is exhausted -- the caller is expected to fall back to
// the provisioning AP rather than spin forever on bad credentials.
esp_err_t wifi_manager_connect_sta(void);

// Arms the runtime monitor (idempotent). Call once from app_main after
// connect_sta() succeeds, before the pager starts. From then on a dropped
// link no longer blind-retries the dead AP: the monitor scans, ranks the
// configured networks by RSSI and reconnects to the best visible one,
// retrying with backoff indefinitely -- the device never reboots into the
// portal on its own. While up it also background-scans periodically and may
// roam proactively when the current AP fades and a configured alternative is
// decisively stronger (threshold/margin/cooldown in app_config.h).
void wifi_manager_start_monitor(void);

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
