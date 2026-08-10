#include "led.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "led_strip.h"
#include "msg_queue.h"

static const char *TAG = "led";

// Small stack: the task only toggles one pixel and sleeps. No TLS, no LVGL,
// no printf on the hot path.
#define LED_TASK_STACK 2048
// Below the button (3) and pager (4) tasks: a blink skipped by a late wake is
// invisible next to the screen lighting up, and the queue depth it reports is
// not latency-sensitive.
#define LED_TASK_PRIO  1

static led_strip_handle_t s_strip;

// Toggles the single pixel between the configured colour and off. Polls the
// queue itself rather than taking a feed of every push/pop, so a missed or
// future call site can never leave the LED stuck on or stuck off.
static void led_task(void *arg)
{
    bool lit = false;
    while (1) {
        if (msg_queue_count() > 0) {
            lit = !lit;
            if (lit) {
                led_strip_set_pixel(s_strip, 0, BOARD_LED_COLOR_R,
                                    BOARD_LED_COLOR_G, BOARD_LED_COLOR_B);
            } else {
                led_strip_clear(s_strip);
            }
            led_strip_refresh(s_strip);
            vTaskDelay(pdMS_TO_TICKS(lit ? BOARD_LED_BLINK_ON_MS
                                         : BOARD_LED_BLINK_OFF_MS));
        } else {
            if (lit) {
                // The queue emptied under us: finish the pulse immediately so
                // the LED does not stay lit for the tail of an on-window.
                led_strip_clear(s_strip);
                led_strip_refresh(s_strip);
                lit = false;
            }
            vTaskDelay(pdMS_TO_TICKS(BOARD_LED_IDLE_POLL_MS));
        }
    }
}

void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_LED_GPIO,
        .max_leds = BOARD_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    ESP_ERROR_CHECK(led_strip_clear(s_strip));

    xTaskCreate(led_task, "led", LED_TASK_STACK, NULL, LED_TASK_PRIO, NULL);
    ESP_LOGI(TAG, "unread indicator on GPIO%d", BOARD_LED_GPIO);
}
