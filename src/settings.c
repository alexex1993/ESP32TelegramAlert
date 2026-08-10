#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app_config.h"

static const char *TAG = "settings";

static const char *NVS_NAMESPACE = "cfg";

#define KEY_PROVISIONED  "provisioned"   // u8: 1 once a complete set is saved
#define KEY_FORCE_AP     "force_ap"      // u8: 1 => boot straight into the portal
#define KEY_BOT_TOKEN    "bot_token"
#define KEY_WIFI_SSID    "wifi_ssid"
#define KEY_WIFI_PASS    "wifi_pass"
#define KEY_TZ           "tz"
#define KEY_LANG         "lang"
#define KEY_PROXY_EN     "proxy_en"
#define KEY_PROXY_HOST   "proxy_host"
#define KEY_PROXY_PORT   "proxy_port"
#define KEY_PROXY_USER   "proxy_user"
#define KEY_PROXY_PASS   "proxy_pass"

// The single in-memory copy, loaded once in settings_load(). Const-viewed by
// the rest of the firmware through settings_get().
static app_settings_t s_settings;

// Helper macros keep the load/save pairs below to one line each and guarantee
// the key name and the destination stay in sync.
#define LOAD_STR(key, dst) \
    do { \
        size_t sz = sizeof(dst); \
        if (nvs_get_str(h, key, dst, &sz) != ESP_OK) { dst[0] = '\0'; ok = false; } \
    } while (0)
#define LOAD_I32(key, dst) \
    do { if (nvs_get_i32(h, key, dst) != ESP_OK) { *(dst) = 0; ok = false; } } while (0)
#define LOAD_U16(key, dst) \
    do { if (nvs_get_u16(h, key, dst) != ESP_OK) { *(dst) = 0; ok = false; } } while (0)
#define LOAD_U8B(key, dst) \
    do { uint8_t _v = 0; nvs_get_u8(h, key, &_v); *(dst) = _v; } while (0)

#define SAVE_STR(key, val) nvs_set_str(h, key, (val)[0] ? (val) : "")
#define SAVE_I32(key, val) nvs_set_i32(h, key, (val))
#define SAVE_U16(key, val) nvs_set_u16(h, key, (val))

bool settings_load(app_settings_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    bool ok = true;

    uint8_t provisioned = 0;
    nvs_get_u8(h, KEY_PROVISIONED, &provisioned);

    LOAD_STR(KEY_BOT_TOKEN, out->bot_token);
    LOAD_STR(KEY_WIFI_SSID, out->wifi_ssid);
    LOAD_STR(KEY_WIFI_PASS, out->wifi_password);
    LOAD_I32(KEY_TZ, &out->tz_offset_hours);
    LOAD_I32(KEY_LANG, &out->ui_language);
    LOAD_U8B(KEY_PROXY_EN, &out->proxy_enabled);
    LOAD_STR(KEY_PROXY_HOST, out->proxy_host);
    LOAD_U16(KEY_PROXY_PORT, &out->proxy_port);
    LOAD_STR(KEY_PROXY_USER, out->proxy_user);
    LOAD_STR(KEY_PROXY_PASS, out->proxy_pass);

    nvs_close(h);

    // Clamp the language: a stale/garbage value must not index past the tables.
    if (out->ui_language != APP_LANG_EN && out->ui_language != APP_LANG_RU) {
        out->ui_language = APP_LANG_EN;
    }
    if (out->tz_offset_hours < -12 || out->tz_offset_hours > 14) {
        out->tz_offset_hours = APP_DEFAULT_TZ_OFFSET_HOURS;
    }

    s_settings = *out;
    bool complete = provisioned && ok &&
                    out->bot_token[0] && out->wifi_ssid[0];
    ESP_LOGI(TAG, "loaded: provisioned=%d complete=%d", provisioned, complete);
    return complete;
}

bool settings_is_provisioned(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t provisioned = 0;
    nvs_get_u8(h, KEY_PROVISIONED, &provisioned);
    nvs_close(h);
    return provisioned != 0;
}

bool settings_force_ap(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    nvs_get_u8(h, KEY_FORCE_AP, &v);
    nvs_close(h);
    return v != 0;
}

const app_settings_t *settings_get(void)
{
    return &s_settings;
}

esp_err_t settings_save(const app_settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    SAVE_STR(KEY_BOT_TOKEN, s->bot_token);
    SAVE_STR(KEY_WIFI_SSID, s->wifi_ssid);
    SAVE_STR(KEY_WIFI_PASS, s->wifi_password);
    SAVE_I32(KEY_TZ, s->tz_offset_hours);
    SAVE_I32(KEY_LANG, s->ui_language);
    nvs_set_u8(h, KEY_PROXY_EN, s->proxy_enabled ? 1 : 0);
    SAVE_STR(KEY_PROXY_HOST, s->proxy_host);
    SAVE_U16(KEY_PROXY_PORT, s->proxy_port);
    SAVE_STR(KEY_PROXY_USER, s->proxy_user);
    SAVE_STR(KEY_PROXY_PASS, s->proxy_pass);

    // Write the flags last and commit: the "provisioned" mark is the single
    // atomic point that flips the device from "open the portal" to "try STA".
    // A power cut before this leaves the set partial -> next boot re-portals.
    nvs_set_u8(h, KEY_FORCE_AP, 0);
    nvs_set_u8(h, KEY_PROVISIONED, 1);
    err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        s_settings = *s;
        ESP_LOGI(TAG, "saved settings, marked provisioned");
    } else {
        ESP_LOGE(TAG, "commit failed: %s", esp_err_to_name(err));
    }
    return err;
}

void settings_request_ap_and_restart(void)
{
    ESP_LOGW(TAG, "force-ap requested, rebooting into portal");
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, KEY_FORCE_AP, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    // Give the UI a beat to show what is happening, then reboot.
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

void settings_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    memset(&s_settings, 0, sizeof(s_settings));
    ESP_LOGI(TAG, "wiped all stored settings");
}
