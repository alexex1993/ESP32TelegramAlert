#pragma once

#include <stdbool.h>

#include "lvgl.h"

// Brings up the SPI bus, the ST7789 panel (in landscape) and LVGL, and starts
// the LVGL timer-handler task. Returns the LVGL display to build a UI on.
// Leaves the backlight on.
lv_display_t *display_init(void);

// Drives the backlight pin only -- the panel keeps its contents and LVGL keeps
// drawing, so lighting it again shows the current screen with nothing to
// redraw. When and why it goes dark is screen.c's business, not this file's.
void display_set_backlight(bool on);

// Finer-grained backlight control via the LEDC peripheral (PWM). The level is
// in 0..DISPLAY_BL_MAX (per-mille of full brightness); the change is applied
// instantly and cancels any in-progress fade. screen.c uses this to drive the
// full/dim/fade schedule.
#define DISPLAY_BL_MAX 1000
void display_set_backlight_level(uint16_t level);

// LVGL is not thread-safe: any lv_* call made from outside the LVGL task
// (the pager task updating labels, the button task clearing a message) must
// be wrapped in display_lvgl_lock()/display_lvgl_unlock().
void display_lvgl_lock(void);
void display_lvgl_unlock(void);

// The TF card shares SPI2 with the panel, and the two cannot be left to the
// SPI driver's own bus lock: a frame flush *queues* its pixel transfer and
// releases the bus while the DMA is still running (esp_lcd's
// panel_io_spi_tx_color), and the driver then hands the freed bus to another
// device without waiting for that transfer to finish. The SD card walking in
// at that moment sets up a transaction on a peripheral that is still busy,
// which trips an assert inside the SPI HAL and reboots the device.
//
// So every SD access is bracketed by these two, and the flush path holds the
// same guard from the moment it hands a buffer to esp_lcd until the
// transfer-done interrupt reports the pixels are out. Only the backlight
// (LEDC) and the RGB LED (RMT) are outside it -- neither touches SPI2.
//
// Held across a whole card operation, so it must never be taken by anything
// that also needs the LVGL lock: the LVGL task waits for this guard *while
// holding* that lock, and the reverse order would deadlock the two.
void display_spi_bus_lock(void);
void display_spi_bus_unlock(void);
