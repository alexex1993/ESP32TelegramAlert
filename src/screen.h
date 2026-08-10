#pragma once

#include <stdbool.h>

// Backlight policy: the glass lights only on a BOOT key press, never on a
// message arriving. An arriving message is signalled by the RGB LED instead
// (see led.h); the screen comes up when the pager is picked up and the key is
// hit. While open it stays lit as long as the queue still holds something to
// acknowledge, then dims after UI_SCREEN_ON_MS of being empty so the last
// receipt can be read. Everything else -- the panel, LVGL, the poll loop --
// keeps running; only the backlight goes dark.

// Creates the auto-off timer. Call after display_init() (which leaves the
// backlight on) and before button_start().
void screen_init(void);

// "A key was pressed": lights the backlight and then either holds it lit
// (there are still unread messages) or starts the UI_SCREEN_ON_MS countdown
// (the queue is empty). The wake press that lights a dark screen routes here
// too.
void screen_activity(void);

// Arms the UI_SCREEN_ON_MS countdown unconditionally, ignoring any waiting
// messages. Used once at boot: a device that restored unread pages from NVS
// must still go dark and wait for a key press rather than lighting up on its
// own, with the RGB LED carrying the "something to read" signal instead.
void screen_arm_off(void);

// False while the backlight is off. The button task uses this so that the
// press which wakes the pager is not also consumed as an acknowledgement.
bool screen_is_on(void);
