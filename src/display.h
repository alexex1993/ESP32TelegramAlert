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
