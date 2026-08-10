#include "screen.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "display.h"
#include "msg_queue.h"

static const char *TAG = "screen";

// Three-phase backlight schedule, driven in software so a key press at any
// moment snaps back to full with no LEDC hardware-fade ISR to race against:
//
//   FULL   for UI_SCREEN_FULL_MS  at UI_SCREEN_FULL_LEVEL,
//   DIM    for UI_SCREEN_DIM_MS   at UI_SCREEN_DIM_LEVEL,
//   FADING over UI_SCREEN_FADE_MS down to 0.
//
// A single one-shot phase timer advances FULL->DIM->"start fading"; once in
// FADING a periodic tick timer steps the level down each FADE_TICK_MS until
// it reaches 0, then stops itself and flips s_on. screen_activity()/
// screen_arm_off() stop both timers and reset to FULL.
//
// The countdown is the only thing that dims the glass now that arriving
// messages no longer hold it lit, so the schedule runs to completion even if
// the queue has since filled (restored pages at boot, or a message that
// landed during the grace window). The RGB LED carries the unread signal in
// that case.

#define FADE_TICK_MS  20  // 100 ticks over a 2 s fade -- smooth, and 50/100 divides evenly

typedef enum { PH_FULL, PH_DIM, PH_FADING, PH_OFF } phase_t;

static esp_timer_handle_t s_phase_timer;  // one-shot: FULL->DIM, DIM->FADING
static esp_timer_handle_t s_fade_timer;   // periodic while in FADING
// Serialises the two writers: screen_activity()/screen_arm_off() from the
// button and pager tasks, and the timer callbacks from the esp_timer task.
// Without it a key press that lights the glass in the same instant a phase
// timer fires could be undone as the callback dims it again.
static SemaphoreHandle_t s_lock;

// display_init() finishes with the backlight lit, and it stays that way
// through Wi-Fi and SNTP; the countdown only starts once the pager is live.
static phase_t s_phase = PH_FULL;
static bool    s_on    = true;
// Per-mille (0..DISPLAY_BL_MAX) so the fade divides evenly into FADE_MS.
static uint16_t s_fade_level;     // current fading target, stepping toward 0
static uint16_t s_fade_step;      // decrement per tick

static uint32_t ms_to_us(uint32_t ms) { return (uint32_t)ms * 1000; }

static void phase_timer_cb(void *arg);
static void fade_tick_cb(void *arg);

// Call with s_lock held.
static void go_full(void)
{
    display_set_backlight_level(UI_SCREEN_FULL_LEVEL);
    s_phase = PH_FULL;
    s_on    = true;
}

// Call with s_lock held. Stops any pending transitions and (re)starts the
// chain at FULL -> DIM -> FADING.
static void restart_chain(void)
{
    esp_timer_stop(s_fade_timer);
    esp_timer_stop(s_phase_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_phase_timer, ms_to_us(UI_SCREEN_FULL_MS)));
}

static void phase_timer_cb(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_phase == PH_FULL) {
        // FULL -> DIM.
        display_set_backlight_level(UI_SCREEN_DIM_LEVEL);
        s_phase = PH_DIM;
        ESP_ERROR_CHECK(esp_timer_start_once(s_phase_timer, ms_to_us(UI_SCREEN_DIM_MS)));
    } else if (s_phase == PH_DIM) {
        // DIM -> FADING. Step down per-mille so 50% over 2 s lands on 0 at
        // exactly 2 s (a 1% integer step would bottom out at ~1 s).
        s_phase      = PH_FADING;
        s_fade_level = UI_SCREEN_DIM_LEVEL;
        uint32_t ticks = UI_SCREEN_FADE_MS / FADE_TICK_MS;
        s_fade_step = (s_fade_level + ticks - 1) / ticks;  // round up -> reaches 0
        ESP_ERROR_CHECK(esp_timer_start_periodic(s_fade_timer, ms_to_us(FADE_TICK_MS)));
    }
    // PH_FADING/PH_OFF: a stray late fire after restart_chain() stopped us.
    xSemaphoreGive(s_lock);
}

static void fade_tick_cb(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_phase != PH_FADING) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (s_fade_level <= s_fade_step) {
        // Last tick: glass is dark.
        s_fade_level = 0;
        display_set_backlight_level(0);
        esp_timer_stop(s_fade_timer);
        s_phase = PH_OFF;
        s_on    = false;
        ESP_LOGI(TAG, "backlight off");
    } else {
        s_fade_level -= s_fade_step;
        display_set_backlight_level(s_fade_level);
    }
    xSemaphoreGive(s_lock);
}

void screen_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock);

    const esp_timer_create_args_t phase_args = {
        .callback = phase_timer_cb,
        .name = "screen_phase",
    };
    const esp_timer_create_args_t fade_args = {
        .callback = fade_tick_cb,
        .name = "screen_fade",
    };
    ESP_ERROR_CHECK(esp_timer_create(&phase_args, &s_phase_timer));
    ESP_ERROR_CHECK(esp_timer_create(&fade_args, &s_fade_timer));
}

void screen_activity(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    go_full();
    restart_chain();
    // Hold the glass lit while there is still something to acknowledge; only
    // start the grace countdown once the queue is empty.
    if (msg_queue_count() != 0) {
        esp_timer_stop(s_phase_timer);
    }

    xSemaphoreGive(s_lock);
}

void screen_arm_off(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Unconditional countdown: see the schedule note above for why the queue
    // depth does not gate this.
    go_full();
    restart_chain();
    xSemaphoreGive(s_lock);
}

bool screen_is_on(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_on;
    xSemaphoreGive(s_lock);
    return on;
}
