#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

#include "app_config.h"
#include "settings.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
// Published by the event handler, read by whoever asks for the link state.
// A single aligned word rather than a formatted string, so a reader on
// another task sees either the old address or the new one, never half of a
// buffer being rewritten under it.
static volatile uint32_t s_ip4_addr;

static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "disconnected, reason=%d", disc ? disc->reason : -1);
        s_ip4_addr = 0;
        if (s_retry_count < APP_WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "retrying WiFi connect (%d/%d)", s_retry_count, APP_WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_ip4_addr = event->ip_info.ip.addr;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    // AP-mode events (AP_START, STAs joining) are intentionally ignored: the
    // portal only needs the AP up and the HTTP server listening, and the
    // device's own AP address is static (read via esp_netif_get_ip_info).
}

void wifi_manager_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));
}

esp_err_t wifi_manager_connect_sta(void)
{
    const app_settings_t *s = settings_get();

    // create_default_wifi_sta is idempotent enough for the single-boot,
    // single-mode lifecycle this firmware uses; track it to be safe.
    static bool s_sta_netif_created = false;
    if (!s_sta_netif_created) {
        esp_netif_create_default_wifi_sta();
        s_sta_netif_created = true;
    }

    s_wifi_event_group = xEventGroupCreate();

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, s->wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, s->wifi_password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    // A prior start_ap() on this boot would have left the radio running; stop
    // before reconfiguring. This path is not used today (AP failure reboots),
    // but the guard keeps the function self-contained.
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to SSID '%s'...", s->wifi_ssid);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "WiFi connection failed after %d retries", APP_WIFI_MAX_RETRY);
    return ESP_FAIL;
}

esp_err_t wifi_manager_start_ap(const char *ssid)
{
    static bool s_ap_netif_created = false;
    if (!s_ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        s_ap_netif_created = true;
    }

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = (uint8_t)strlen(ssid);
    ap_config.ap.channel = APP_PROVISION_AP_CHANNEL;
    ap_config.ap.password[0] = '\0';          // open network, per the brief
    ap_config.ap.max_connection = 2;          // phone + maybe a laptop
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Advertise ourselves as the DNS server so a phone that joins the AP sends
    // its captive-portal probes (clients3.google.com/generate_204,
    // captive.apple.com/...) to the ESP, where provision.c's hijack responder
    // answers every A query with this address and the OS pops the sign-in
    // sheet automatically. Without this the probe would not resolve and the
    // popup would not appear (the URL on screen still works by hand).
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif) {
        esp_netif_ip_info_t ipinfo;
        esp_netif_dns_info_t dns;
        memset(&dns, 0, sizeof(dns));
        if (esp_netif_get_ip_info(netif, &ipinfo) == ESP_OK) {
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4 = ipinfo.ip;
            esp_netif_dhcps_stop(netif);
            esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                   &dns, sizeof(dns));
            esp_netif_dhcps_start(netif);
        }
    }

    ESP_LOGI(TAG, "SoftAP '%s' up (open), DNS hijack armed, waiting for a client on the portal", ssid);
    return ESP_OK;
}

void wifi_manager_get_ap_ip(char *buf, size_t size)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!netif) {
        snprintf(buf, size, "192.168.4.1");
        return;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        snprintf(buf, size, "192.168.4.1");
        return;
    }
    snprintf(buf, size, IPSTR, IP2STR(&ip.ip));
}

void wifi_manager_get_info(wifi_manager_info_t *out)
{
    memset(out, 0, sizeof(*out));

    wifi_ap_record_t ap;
    bool associated = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
    if (associated) {
        // ssid is a uint8_t[33] the driver already NUL-terminates.
        strlcpy(out->ssid, (const char *)ap.ssid, sizeof(out->ssid));
        out->rssi = ap.rssi;
        out->channel = ap.primary;
    }

    esp_ip4_addr_t addr = { .addr = s_ip4_addr };
    if (addr.addr != 0) {
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&addr));
    }

    // Associated without a lease is a half-up link, and saying "connected"
    // there would be a lie -- nothing can be sent over it.
    out->connected = associated && addr.addr != 0;
}
