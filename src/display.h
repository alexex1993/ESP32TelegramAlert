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

// LVGL is not thread-safe: any lv_* call made from outside the LVGL task
// (the pager task updating labels, the button task clearing a message) must
// be wrapped in display_lvgl_lock()/display_lvgl_unlock().
void display_lvgl_lock(void);
void display_lvgl_unlock(void);
