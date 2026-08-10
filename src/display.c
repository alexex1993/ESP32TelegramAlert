#include "display.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/lock.h>
#include <sys/param.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "display";

// 20 lines of a 320px-wide landscape frame is ~12.8 kB per buffer. Going
// wider buys little and competes with the TLS session for internal RAM.
#define LVGL_DRAW_BUF_LINES    20
#define LVGL_TICK_PERIOD_MS    2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS (1000 / CONFIG_FREERTOS_HZ)
#define LVGL_TASK_STACK_SIZE   (6 * 1024)
#define LVGL_TASK_PRIORITY     2

static _lock_t s_lvgl_lock;

// Backlight is dimmed through the LEDC peripheral so the eye sees a smooth
// ramp rather than a hard on/off. The same pin (BOARD_LCD_PIN_BL) is routed
// via the GPIO matrix to LEDC channel 0; the duty cycle is scaled from the
// 0..DISPLAY_BL_MAX API space into the LEDC's 13-bit range.
#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BL_LEDC_DUTY_RES    LEDC_TIMER_13_BIT  // 0..8191
#define BL_LEDC_DUTY_MAX    ((1U << 13) - 1)
#define BL_LEDC_FREQ_HZ     5000

static inline uint32_t bl_level_to_duty(uint16_t level)
{
    if (level == 0) {
        return 0;
    }
    if (level >= DISPLAY_BL_MAX) {
        return BL_LEDC_DUTY_MAX;
    }
    return ((uint32_t)BL_LEDC_DUTY_MAX * level) / DISPLAY_BL_MAX;
}

void display_set_backlight_level(uint16_t level)
{
    uint32_t duty = bl_level_to_duty(level);
    // An instant duty update overrides whatever the panel was doing; screen.c
    // drives the fade in software, so no LEDC hardware fade is in flight here.
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

void display_set_backlight(bool on)
{
    display_set_backlight_level(on ? DISPLAY_BL_MAX : 0);
}

void display_lvgl_lock(void)
{
    _lock_acquire(&s_lvgl_lock);
}

void display_lvgl_unlock(void)
{
    _lock_release(&s_lvgl_lock);
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    // SPI LCDs expect big-endian RGB565; LVGL's software renderer produces
    // little-endian, so swap bytes before handing the buffer to the panel.
    lv_draw_sw_rgb565_swap(px_map, (offsetx2 + 1 - offsetx1) * (offsety2 + 1 - offsety1));
#if BOARD_LCD_SELFTEST
    // First few flushes only: says whether LVGL renders at all, what area it
    // hands over, and what colour it actually produced.
    static int s_flush_log_budget = 12;
    if (s_flush_log_budget > 0) {
        s_flush_log_budget--;
        ESP_LOGW(TAG, "flush (%d,%d)-(%d,%d) first_px=0x%04X", offsetx1, offsety1, offsetx2,
                 offsety2, ((uint16_t *)px_map)[0]);
    }
#endif
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
}

#if BOARD_LCD_SELFTEST
static void panel_selftest(esp_lcd_panel_handle_t panel)
{
    // Byte-swapped RGB565, same order the flush callback sends: the panel
    // wants big-endian, so red 0xF800 goes out as the bytes F8 00.
    static const uint16_t colors[] = { 0x00F8, 0xE007, 0x1F00 };
    const int lines = 8;
    uint16_t *buf = spi_bus_dma_memory_alloc(BOARD_LCD_SPI_HOST,
                                              BOARD_LCD_H_RES * lines * sizeof(uint16_t), 0);
    assert(buf);

    for (size_t c = 0; c < sizeof(colors) / sizeof(colors[0]); c++) {
        for (int i = 0; i < BOARD_LCD_H_RES * lines; i++) {
            buf[i] = colors[c];
        }
        for (int y = 0; y < BOARD_LCD_V_RES; y += lines) {
            int h = MIN(lines, BOARD_LCD_V_RES - y);
            ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, BOARD_LCD_H_RES, y + h, buf));
        }
        ESP_LOGW(TAG, "selftest: filled %dx%d with 0x%04X", BOARD_LCD_H_RES, BOARD_LCD_V_RES,
                 colors[c]);
        // Also lets the queued SPI transfers finish before the buffer is freed.
        vTaskDelay(pdMS_TO_TICKS(700));
    }

    free(buf);
}
#endif

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_port_task(void *arg)
{
    uint32_t time_till_next_ms = 0;
    while (1) {
        display_lvgl_lock();
        time_till_next_ms = lv_timer_handler();
        display_lvgl_unlock();
        time_till_next_ms = MAX(time_till_next_ms, LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

lv_display_t *display_init(void)
{
    ESP_LOGI(TAG, "configure backlight LEDC");
    // The board's BL pin is active high (BOARD_LCD_BL_ON_LEVEL == 1); route it
    // through LEDC for PWM dimming. Starting at duty 0 keeps the glass dark
    // through SPI/LVGL bring-up, matching the original GPIO behaviour.
    ledc_timer_config_t bl_timer_cfg = {
        .speed_mode      = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_DUTY_RES,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer_cfg));
    ledc_channel_config_t bl_ch_cfg = {
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = BOARD_LCD_PIN_BL,
        .duty       = 0,
        .hpoint     = 0,
        .flags.output_invert = !BOARD_LCD_BL_ON_LEVEL,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch_cfg));

    ESP_LOGI(TAG, "initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        // The TF card shares this bus and reads through its MISO line
        // (BOARD_SD_PIN_MISO == GPIO5). The write-only LCD never reads, so
        // declaring the pin here is free for the panel but mandatory for the
        // SDSPI device that sd_log.c attaches afterwards.
        .miso_io_num = BOARD_SD_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        .pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = BOARD_LCD_CMD_BITS,
        .lcd_param_bits = BOARD_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST,
                                              &io_config, &io_handle));

    ESP_LOGI(TAG, "install ST7789 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, BOARD_LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

#if BOARD_LCD_SELFTEST
    display_set_backlight_level(DISPLAY_BL_MAX);
    panel_selftest(panel_handle);
#endif

    ESP_LOGI(TAG, "initialize LVGL (%dx%d landscape)", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_init();

    lv_display_t *display = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    size_t draw_buffer_sz = BOARD_LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(BOARD_LCD_SPI_HOST, draw_buffer_sz, 0);
    void *buf2 = spi_bus_dma_memory_alloc(BOARD_LCD_SPI_HOST, draw_buffer_sz, 0);
    assert(buf1 && buf2);
    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display, panel_handle);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display));

    xTaskCreate(lvgl_port_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "backlight on");
    display_set_backlight_level(DISPLAY_BL_MAX);

#if BOARD_LCD_SELFTEST
    // The same idea as panel_selftest, but driven through LVGL: magenta is
    // distinct from every colour above, so if it appears the whole pipeline
    // works and only the UI's own styling is at fault.
    display_lvgl_lock();
    lv_obj_t *test_scr = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(test_scr, lv_color_hex(0xFF00FF), 0);
    lv_obj_set_style_bg_opa(test_scr, LV_OPA_COVER, 0);
    display_lvgl_unlock();
    ESP_LOGW(TAG, "selftest: LVGL screen set to magenta");
    vTaskDelay(pdMS_TO_TICKS(1500));
#endif

    return display;
}
