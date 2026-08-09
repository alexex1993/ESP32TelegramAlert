#pragma once

#include "lvgl.h"

// Each of these takes the LVGL lock itself, so callers on any task may use
// them freely -- but must not already hold it.

// Builds the pager screen. Call once, before any other ui_* function.
void ui_init(lv_display_t *disp);

// Repaints the header and message area from the current queue contents.
void ui_render_queue(void);

// Sets the footer line: boot progress, then connection and receipt status.
void ui_set_status(const char *text);
