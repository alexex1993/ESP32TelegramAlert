#include "screen.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "display.h"
#include "msg_queue.h"

static const char *TAG = "screen";

static esp_timer_handle_t s_off_timer;
// display_init() finishes with the backlight lit, and it stays that way
// through Wi-Fi and SNTP; the countdown only starts once the pager is live.
static bool s_on = true;
// Serialises the two writers: screen_activity() from the button and pager
// tasks, and the timer callback from the esp_timer task. Without it a message
// arriving in the same instant the timer fires can be paged onto a screen that
// is then switched off behind it.
static SemaphoreHandle_t s_lock;

// Call with s_lock held.
static void set_on(bool on)
{
    if (on == s_on) {
        return;
    }
    s_on = on;
    display_set_backlight(on);
    ESP_LOGI(TAG, "backlight %s", on ? "on" : "off");
}

static void off_timer_cb(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // A message can land between the timer expiring and this running, and an
    // unread message outranks the countdown.
    if (msg_queue_count() == 0) {
        set_on(false);
    }
    xSemaphoreGive(s_lock);
}

void screen_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock);

    const esp_timer_create_args_t args = {
        .callback = off_timer_cb,
        .name = "screen_off",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_off_timer));
}

void screen_activity(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    set_on(true);
    // Not running is the common case and not an error, so the return value is
    // deliberately ignored.
    esp_timer_stop(s_off_timer);
    if (msg_queue_count() == 0) {
        ESP_ERROR_CHECK(esp_timer_start_once(s_off_timer, (uint64_t)UI_SCREEN_ON_MS * 1000));
    }

    xSemaphoreGive(s_lock);
}

bool screen_is_on(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_on;
    xSemaphoreGive(s_lock);
    return on;
}
