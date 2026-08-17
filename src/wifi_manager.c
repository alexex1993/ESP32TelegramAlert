#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

#include "app_config.h"
#include "settings.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_LINK_LOST_BIT  BIT2

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
// Published by the event handler, read by whoever asks for the link state.
// A single aligned word rather than a formatted string, so a reader on
// another task sees either the old address or the new one, never half of a
// buffer being rewritten under it.
static volatile uint32_t s_ip4_addr;

// ---- runtime monitor state (see wifi_manager_start_monitor) ----
// Armed by main.c right after the boot connect succeeds. Before that the
// handler does its boot-time job (retry the same AP, then FAIL so the boot
// loop moves to the next configured network); after that a disconnect is the
// monitor's business and the handler only flags it.
static volatile bool s_monitor_armed = false;
static volatile bool s_link_up = false;
static int s_current_slot = -1;      // index of the network last configured
static int64_t s_last_switch_ms = 0; // roam cooldown anchor

static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "disconnected, reason=%d", disc ? disc->reason : -1);
        s_ip4_addr = 0;
        if (s_monitor_armed) {
            // Runtime drop: no blind retries here -- the monitor task scans,
            // picks the best configured network that is actually on air and
            // reconnects. Clearing CONNECTED keeps the group truthful so a
            // later wait cannot mistake a stale bit for a live link.
            s_link_up = false;
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            xEventGroupSetBits(s_wifi_event_group, WIFI_LINK_LOST_BIT);
        } else if (s_retry_count < APP_WIFI_MAX_RETRY) {
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
        s_link_up = true;
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

// Shared by the boot loop and the runtime failover so both configure a
// candidate network identically.
static void sta_config_from_slot(wifi_config_t *cfg, int slot)
{
    const app_settings_t *s = settings_get();
    memset(cfg, 0, sizeof(*cfg));
    strlcpy((char *)cfg->sta.ssid, s->wifi_ssid[slot], sizeof(cfg->sta.ssid));
    strlcpy((char *)cfg->sta.password, s->wifi_password[slot], sizeof(cfg->sta.password));
    cfg->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg->sta.pmf_cfg.capable = true;
    cfg->sta.pmf_cfg.required = false;
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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Walk the configured networks in order until one comes up. Each attempt
    // reconfigures the STA interface and restarts the radio; STA_START then
    // fires the first esp_wifi_connect() from the event handler, whose retry
    // and fail bookkeeping is reset per attempt below (before the start, so an
    // early event cannot carry a stale count or bits into the wait).
    for (int i = 0; i < APP_SETTINGS_WIFI_NETS_MAX; i++) {
        if (s->wifi_ssid[i][0] == '\0') {
            break;   // slots are compacted at save time; empty = end of list
        }

        wifi_config_t wifi_config;
        sta_config_from_slot(&wifi_config, i);

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        // A prior start (an earlier attempt in this loop, or a never-used AP
        // start) would have left the radio running; stop before restarting.
        esp_wifi_stop();

        s_retry_count = 0;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "connecting to SSID '%s' (network %d)...", s->wifi_ssid[i], i + 1);
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (bits & WIFI_CONNECTED_BIT) {
            s_current_slot = i;
            ESP_LOGI(TAG, "WiFi connected to '%s'", s->wifi_ssid[i]);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "SSID '%s' failed after %d retries, trying the next network",
                 s->wifi_ssid[i], APP_WIFI_MAX_RETRY);
    }

    ESP_LOGE(TAG, "no configured WiFi network connected");
    return ESP_FAIL;
}

// ------------------------------------------------------- runtime monitor
// One task, two jobs (wifi_manager_start_monitor arms it after the boot
// connect succeeds):
//
//   while up   -- scan every APP_WIFI_MONITOR_PERIOD_MS so the set of
//                 reachable configured networks is known *before* it is
//                 needed, and roam proactively if the current AP is dying
//                 and a configured alternative is decisively stronger;
//   on a drop  -- scan, rank the configured networks by RSSI, connect to the
//                 best visible one, retry with backoff until one answers.
//
// The pager needs no cooperation: its poll fails during the outage, shows the
// offline status and backs off; once the link is back it reconnects by itself.

// Blocking sweep. Returns records sorted by RSSI (strongest first, as the
// driver stores them), or a negative value if the scan could not run.
static int scan_once(wifi_ap_record_t *recs, int max)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return -1;
    }
    uint16_t n = (uint16_t)max;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        return -1;
    }
    return (int)n;
}

// Scan and rank the *configured* networks by current on-air RSSI. Fills
// slots[]/rssis[] as parallel arrays (rank r: slots[r] is the configured slot
// and rssis[r] its signal), strongest first; mesh/repeater nodes sharing an
// SSID collapse into the strongest one. Returns the entry count.
static int scan_rank_configured(int *slots, int8_t *rssis)
{
    const app_settings_t *s = settings_get();
    wifi_ap_record_t recs[16];
    int n = scan_once(recs, (int)(sizeof(recs) / sizeof(recs[0])));
    if (n < 0) {
        return 0;
    }

    bool seen[APP_SETTINGS_WIFI_NETS_MAX] = { false };
    int8_t slot_rssi[APP_SETTINGS_WIFI_NETS_MAX] = { 0 };
    for (int r = 0; r < n; r++) {
        for (int i = 0; i < APP_SETTINGS_WIFI_NETS_MAX; i++) {
            if (!seen[i] && s->wifi_ssid[i][0] &&
                strcmp((const char *)recs[r].ssid, s->wifi_ssid[i]) == 0) {
                seen[i] = true;
                slot_rssi[i] = recs[r].rssi;   // records come strongest-first
            }
        }
    }

    int count = 0;
    for (int i = 0; i < APP_SETTINGS_WIFI_NETS_MAX; i++) {
        if (!seen[i]) {
            continue;
        }
        int j = count++;   // insertion sort, strongest RSSI first
        while (j > 0 && rssis[j - 1] < slot_rssi[i]) {
            rssis[j] = rssis[j - 1];
            slots[j] = slots[j - 1];
            j--;
        }
        rssis[j] = slot_rssi[i];
        slots[j] = i;
    }
    return count;
}

// Point the STA at configured slot `slot` and connect, waiting up to
// APP_WIFI_CONNECT_WAIT_MS for an IP. The explicit disconnect matters for the
// roam case: esp_wifi_connect() on an associated station does nothing.
static bool try_network(int slot)
{
    wifi_config_t cfg;
    sta_config_from_slot(&cfg, slot);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    s_current_slot = slot;
    esp_wifi_disconnect();
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(APP_WIFI_CONNECT_WAIT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        // The disconnect above went through the event handler, which flagged
        // the monitor exactly as a real drop would. Clear it now that the link
        // is back: leaving it set makes the monitor's own wait return
        // immediately on the next lap and burn a full scan on a link that just
        // came up under it. s_link_up is re-read after the clear because a
        // genuine drop landing in that window would otherwise have its flag
        // wiped here and go unnoticed until the next periodic scan.
        xEventGroupClearBits(s_wifi_event_group, WIFI_LINK_LOST_BIT);
        if (!s_link_up) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_LINK_LOST_BIT);
        }
        s_last_switch_ms = esp_timer_get_time() / 1000;
        return true;
    }
    return false;
}

// Proactive half: refresh the picture of what is on air, and if the current
// AP has sunk to a level where pages would start dropping while another
// configured network is decisively stronger, switch before the link dies on
// its own. Margin and cooldown are the anti-flap.
static void monitor_scan_and_maybe_roam(void)
{
    int slots[APP_SETTINGS_WIFI_NETS_MAX];
    int8_t rssis[APP_SETTINGS_WIFI_NETS_MAX];
    int n = scan_rank_configured(slots, rssis);
    if (n <= 0) {
        return;
    }

    int cur = -1;
    for (int i = 0; i < n; i++) {
        if (slots[i] == s_current_slot) {
            cur = i;
            break;
        }
    }
    if (cur < 0 || rssis[cur] > APP_WIFI_ROAM_RSSI_THRESHOLD_DBM || cur == 0) {
        return;   // current AP healthy (or already the strongest), stay put
    }
    if (rssis[0] < rssis[cur] + APP_WIFI_ROAM_MARGIN_DB) {
        return;   // alternative not decisively better
    }
    if (esp_timer_get_time() / 1000 - s_last_switch_ms < APP_WIFI_ROAM_COOLDOWN_MS / 1000) {
        return;
    }

    ESP_LOGW(TAG, "roaming: '%s' at %d dBm, '%s' at %d dBm is stronger",
             settings_get()->wifi_ssid[s_current_slot], rssis[cur],
             settings_get()->wifi_ssid[slots[0]], rssis[0]);
    try_network(slots[0]);
}

static void monitor_task(void *arg)
{
    (void)arg;
    int backoff_ms = APP_WIFI_FAILOVER_BACKOFF_MIN_MS;

    while (1) {
        // Sleep until the link drops, or until it is time for the next
        // proactive scan, whichever comes first.
        xEventGroupWaitBits(s_wifi_event_group, WIFI_LINK_LOST_BIT,
                            pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(APP_WIFI_MONITOR_PERIOD_MS));

        if (s_link_up) {
            monitor_scan_and_maybe_roam();
            continue;
        }

        // Link lost: scan, then try the visible configured networks best
        // first (the dead one included -- a rebooting AP is often back by the
        // time the scan finishes, and it may still be the strongest).
        ESP_LOGW(TAG, "link lost, failing over to another configured network");
        while (!s_link_up) {
            int slots[APP_SETTINGS_WIFI_NETS_MAX];
            int8_t rssis[APP_SETTINGS_WIFI_NETS_MAX];
            int n = scan_rank_configured(slots, rssis);
            bool ok = false;
            for (int i = 0; i < n && !ok; i++) {
                ESP_LOGI(TAG, "failover: trying '%s' (%d dBm)",
                         settings_get()->wifi_ssid[slots[i]], rssis[i]);
                ok = try_network(slots[i]);
            }
            if (!ok) {
                // Nothing visible answered (or nothing was visible at all):
                // back off, rescan, try again. Forever, not into the portal --
                // the pager stays alive offline and the networks may return.
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms *= 2;
                if (backoff_ms > APP_WIFI_FAILOVER_BACKOFF_MAX_MS) {
                    backoff_ms = APP_WIFI_FAILOVER_BACKOFF_MAX_MS;
                }
            }
        }
        backoff_ms = APP_WIFI_FAILOVER_BACKOFF_MIN_MS;
        ESP_LOGI(TAG, "link restored on '%s'", settings_get()->wifi_ssid[s_current_slot]);
    }
}

void wifi_manager_start_monitor(void)
{
    static bool s_monitor_started = false;
    if (s_monitor_started) {
        return;
    }
    s_monitor_started = true;
    s_monitor_armed = true;
    // Small stack: the task only scans (driver-internal buffering), ranks
    // three slots and logs. Same priority as the LVGL task -- the work is
    // rare and short, and it must never starve the pager itself.
    xTaskCreate(monitor_task, "wifi_mon", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "runtime monitor armed: %d s scans, failover on link loss",
             APP_WIFI_MONITOR_PERIOD_MS / 1000);
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
