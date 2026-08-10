#pragma once

#include <stdarg.h>

#include "lvgl.h"

// Each of these takes the LVGL lock itself, so callers on any task may use
// them freely -- but must not already hold it.

// Builds the pager screen. Call once, before any other ui_* function.
void ui_init(lv_display_t *disp);

// Repaints the header and message area from the current queue contents.
void ui_render_queue(void);

// Sets the footer line: boot progress, then connection and receipt status.
// `text` must outlive the call (it is copied into the label).
void ui_set_status(const char *text);

// printf-flavoured ui_set_status: formats into an internal buffer and sets the
// footer. Needed because STR_* are now runtime lookups (no longer literals),
// so they cannot be string-literal-concatenated with adjacent text or icons
// at the call site -- format with %s instead.
void ui_set_statusf(const char *fmt, ...);

// Replaces the message area with the captive-portal instructions: the open AP
// name and the URL to open. Shown only during first-boot provisioning, before
// the pager task exists. Re-entrant-safe (takes the LVGL lock).
void ui_show_provision(const char *ap_ssid, const char *url);
