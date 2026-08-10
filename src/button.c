#include "button.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "button";

#define BUTTON_POLL_MS      10
// The press callback runs on this task and holds a popped pager_msg_t live
// across ui_render_queue(), which peeks another one -- two APP_MSG_TEXT_MAX
// buffers on the stack at once.
#define BUTTON_TASK_STACK   8192
#define BUTTON_TASK_PRIO    3

static button_press_cb_t s_on_press;
static button_long_cb_t  s_on_long;

static inline bool button_is_down(void)
{
    return gpio_get_level(BOARD_BUTTON_GPIO) == BOARD_BUTTON_ACTIVE_LEVEL;
}

// Polling rather than an interrupt: presses are a human-scale event, and this
// keeps debouncing to a plain state machine with no ISR-safety constraints on
// the callback (which sends a Telegram receipt and repaints the screen).
static void button_task(void *arg)
{
    bool was_down = false;
    int stable_ms = 0;
    bool down_since_release = false;
    int hold_ms = 0;
    bool long_fired = false;

    while (1) {
        bool is_down = button_is_down();

        if (is_down == was_down) {
            stable_ms = 0;
        } else {
            stable_ms += BUTTON_POLL_MS;
            if (stable_ms >= BOARD_BUTTON_DEBOUNCE_MS) {
                was_down = is_down;
                stable_ms = 0;
                if (is_down) {
                    // Fire on press, not release: the pager should react the
                    // instant the key goes down.
                    down_since_release = true;
                    hold_ms = 0;
                    long_fired = false;
                    ESP_LOGI(TAG, "press");
                    if (s_on_press) {
                        s_on_press();
                    }
                } else {
                    down_since_release = false;
                }
            }
        }

        // Long-press tracking runs independently of the edge debounce: once the
        // key is down, accumulate held time and trip the long callback once at
        // the threshold. The short press already fired above, so a long hold is
        // "short press happened, then continued" -- acceptable for a gesture
        // that ends in a reboot anyway.
        if (down_since_release && !long_fired && s_on_long) {
            hold_ms += BUTTON_POLL_MS;
            if (hold_ms >= APP_BUTTON_LONG_HOLD_MS) {
                long_fired = true;
                ESP_LOGI(TAG, "long press (%d ms), invoking long callback", APP_BUTTON_LONG_HOLD_MS);
                s_on_long();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

void button_start(button_press_cb_t on_press, button_long_cb_t on_long)
{
    s_on_press = on_press;
    s_on_long  = on_long;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (BOARD_BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (BOARD_BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    xTaskCreate(button_task, "button", BUTTON_TASK_STACK, NULL, BUTTON_TASK_PRIO, NULL);
    ESP_LOGI(TAG, "watching GPIO%d", BOARD_BUTTON_GPIO);
}
