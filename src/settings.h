#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Runtime device settings, persisted in NVS under the "cfg" namespace and
// edited through the captive-portal web page (see provision.c). This replaces
// the old compile-time secrets.h: one firmware image now flashes to every
// device, and each is configured over WiFi on first boot.
//
// The settings are loaded once into RAM at boot (settings_load) and afterwards
// read through the const pointer settings_get() returns -- there is no per-call
// NVS traffic on the hot paths (the Telegram poll, the receipts).

#define APP_SETTINGS_BOT_TOKEN_MAX  96
#define APP_SETTINGS_WIFI_SSID_MAX  33
#define APP_SETTINGS_WIFI_PASS_MAX  64
#define APP_SETTINGS_PROXY_HOST_MAX 128
#define APP_SETTINGS_PROXY_USER_MAX 96

#define APP_LANG_EN 0
#define APP_LANG_RU 1

typedef struct {
    char     bot_token[APP_SETTINGS_BOT_TOKEN_MAX];   // "<digits>:<aa...>"
    char     wifi_ssid[APP_SETTINGS_WIFI_SSID_MAX];
    char     wifi_password[APP_SETTINGS_WIFI_PASS_MAX];
    int32_t  tz_offset_hours;                          // added to UTC for [HH:MM]
    int32_t  ui_language;                              // APP_LANG_EN / APP_LANG_RU
    bool     proxy_enabled;
    char     proxy_host[APP_SETTINGS_PROXY_HOST_MAX];
    uint16_t proxy_port;
    char     proxy_user[APP_SETTINGS_PROXY_USER_MAX];  // empty => no RFC 1929 auth
    char     proxy_pass[APP_SETTINGS_PROXY_USER_MAX];
} app_settings_t;

// Loads settings from NVS into the in-memory copy. Returns true when a complete
// set is present (the "provisioned" flag set); false on first boot, after a
// clear, or when the stored set is incomplete -- in all those cases the caller
// is expected to bring up the provisioning AP instead of trying to connect.
//
// On a false return the struct is still filled with whatever individual fields
// existed (so the portal form can pre-fill a partial / previous attempt); any
// field that was absent is left as its zero value.
bool settings_load(app_settings_t *out);

// True iff a complete settings set is stored. Cheaper than a full load: just
// checks the "provisioned" flag.
bool settings_is_provisioned(void);

// True iff the device was asked to skip STA and go straight to the AP (either
// a long BOOT hold requested re-provisioning, or the last STA attempt failed).
// The flag is sticky across the reboot that routes the device into the portal
// but is cleared by settings_save(), so submitting the form always retries STA.
bool settings_force_ap(void);

// Pointer to the settings loaded at boot. const: nothing mutates them at
// runtime, only the provisioning flow writes (via settings_save), and that ends
// in a reboot.
const app_settings_t *settings_get(void);

// Persists a full settings set, marks the set provisioned and clears any
// force-ap flag. Safe to call from the http-server task. Callers should
// esp_restart() shortly after -- the running network stack still holds the old
// values and the new ones only take effect on the next boot.
esp_err_t settings_save(const app_settings_t *s);

// Sets the force-ap flag and reboots, without touching the stored fields: used
// by the long-press gesture so the portal re-opens pre-filled with the last
// attempt. Never returns.
void settings_request_ap_and_restart(void);

// Erases every stored field and the flags. Used only by a factory wipe gesture
// if ever needed; the normal re-provision path is settings_request_ap_and_restart.
void settings_clear(void);
