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
// Serialises the two writers: screen_activity()/screen_arm_off() from the
// button and pager tasks, and the timer callback from the esp_timer task.
// Without it a key press that lights the glass in the same instant the
// grace timer fires could be undone as the callback dims it again.
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
    // The countdown is the only thing that dims the glass now that arriving
    // messages no longer hold it lit, so when it fires the screen goes dark
    // unconditionally -- even if the queue has since filled (restored pages at
    // boot, or a message that landed during the grace window). The RGB LED
    // carries the unread signal in that case.
    set_on(false);
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
    // Hold the glass lit while there is still something to acknowledge; only
    // start the grace countdown once the queue is empty.
    // Not running is the common case and not an error, so the return value is
    // deliberately ignored.
    esp_timer_stop(s_off_timer);
    if (msg_queue_count() == 0) {
        ESP_ERROR_CHECK(esp_timer_start_once(s_off_timer, (uint64_t)UI_SCREEN_ON_MS * 1000));
    }

    xSemaphoreGive(s_lock);
}

void screen_arm_off(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_timer_stop(s_off_timer);
    // Unconditional countdown: see off_timer_cb for why the queue depth does
    // not gate this.
    ESP_ERROR_CHECK(esp_timer_start_once(s_off_timer, (uint64_t)UI_SCREEN_ON_MS * 1000));
    xSemaphoreGive(s_lock);
}

bool screen_is_on(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_on;
    xSemaphoreGive(s_lock);
    return on;
}
