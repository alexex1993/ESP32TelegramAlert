#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Brings up the station interface and blocks until an IP is acquired or
// APP_WIFI_MAX_RETRY attempts have failed.
esp_err_t wifi_manager_connect_blocking(void);

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
