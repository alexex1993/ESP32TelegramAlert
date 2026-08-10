#pragma once

#include <stdbool.h>

// Backlight policy: the glass is lit only while there is something to read.
//
// The rule is "on if and only if the message queue is non-empty", with a
// grace window on top of it so the screen does not blink out the instant the
// last message is acknowledged, and so the BOOT key can be used to look at an
// empty pager. Everything else -- the panel, LVGL, the poll loop -- keeps
// running; only the backlight goes dark.

// Creates the auto-off timer. Call after display_init() (which leaves the
// backlight on) and before button_start().
void screen_init(void);

// "Something happened worth looking at": a message arrived, a key was pressed,
// a receipt went out. Lights the backlight, and then either holds it lit (there
// are unread messages) or starts the UI_SCREEN_ON_MS countdown (there are not).
void screen_activity(void);

// False while the backlight is off. The button task uses this so that the
// press which wakes the pager is not also consumed as an acknowledgement.
bool screen_is_on(void);
