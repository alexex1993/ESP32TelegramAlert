#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "display.h"
#include "msg_queue.h"
#include "pager_task.h"
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

    lv_display_t *disp = display_init();
    ui_init(disp);
    ui_set_status(STR_WIFI_CONNECTING);

    if (wifi_manager_connect_blocking() != ESP_OK) {
        ui_set_status(STR_WIFI_FAILED);
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    ui_set_status(STR_TIME_SYNCING);
    sync_time();

    pager_start();
}
