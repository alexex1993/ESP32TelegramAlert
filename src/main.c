#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "display.h"
#include "led.h"
#include "msg_queue.h"
#include "pager_task.h"
#include "provision.h"
#include "sd_log.h"
#include "settings.h"
#include "ui.h"
#include "ui_strings.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static void sync_time(void)
{
    // Only cosmetic: message timestamps come from Telegram, and ESP-IDF's
    // mbedTLS does not check certificate validity dates by default, so a
    // failed sync costs nothing but a slightly odd log clock.
    ESP_LOGI(TAG, "syncing time via SNTP...");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync timed out, continuing with unsynced clock");
    } else {
        ESP_LOGI(TAG, "time synced");
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    msg_queue_init();
    // Started before Wi-Fi so a device that restored unread pages from NVS
    // begins blinking immediately, not after the network comes up.
    led_init();

    lv_display_t *disp = display_init();
    ui_init(disp);

    // Load whatever settings are stored (may be empty on first boot). The
    // language is applied right away so even the provisioning screen speaks the
    // right language if a partial set was saved before.
    app_settings_t loaded;
    bool provisioned = settings_load(&loaded);
    ui_set_language(settings_get()->ui_language);

    wifi_manager_init();

    // Enter the portal on first boot, after a re-provision gesture, or after a
    // prior STA attempt gave up. provision_start() does not return: it runs the
    // AP + DNS hijack + HTTP server until a successful save reboots the device.
    if (!provisioned || settings_force_ap()) {
        provision_start();
        return;   // unreachable -- provision_start blocks forever
    }

    ui_set_status(STR_WIFI_CONNECTING);
    if (wifi_manager_connect_sta() != ESP_OK) {
        // Bad credentials (or the network moved). Fall back to the portal
        // pre-filled with the last attempt rather than boot-loop on a dead
        // link: set the force-ap flag and reboot into provisioning.
        ESP_LOGW(TAG, "STA connect failed, falling back to provisioning portal");
        ui_set_status(STR_STA_FAILED_AP);
        vTaskDelay(pdMS_TO_TICKS(1500));
        settings_request_ap_and_restart();
        return;   // unreachable
    }

    ui_set_status(STR_TIME_SYNCING);
    sync_time();

    // Mounts the TF card on the LCD's shared SPI bus; best-effort, the pager
    // runs without it. After this, sd_log_message() logs each paged message to
    // its own file under /sdcard/TelegramPager/<chat_id>/<date>/<time>.txt.
    sd_log_init();

    pager_start();
}
