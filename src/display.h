#pragma once

#include "lvgl.h"

// Brings up the SPI bus, the ST7789 panel (in landscape) and LVGL, and starts
// the LVGL timer-handler task. Returns the LVGL display to build a UI on.
lv_display_t *display_init(void);

// LVGL is not thread-safe: any lv_* call made from outside the LVGL task
// (the pager task updating labels, the button task clearing a message) must
// be wrapped in display_lvgl_lock()/display_lvgl_unlock().
void display_lvgl_lock(void);
void display_lvgl_unlock(void);
