#pragma once

typedef void (*button_press_cb_t)(void);
typedef void (*button_long_cb_t)(void);

// Starts a task that watches the BOOT key. `on_press` is called once per short
// press (immediately on the down edge, for an instant reaction); `on_long`, if
// non-NULL, is called once when the key has been held continuously for
// APP_BUTTON_LONG_HOLD_MS. A hold that fires the long callback does not
// suppress the short one -- the short already went out on the down edge -- so
// the long gesture is layered on top, used for maintenance (re-provisioning)
// rather than paging.
//
// Both callbacks run on the button task's context, so they may block briefly
// and may take the LVGL lock.
void button_start(button_press_cb_t on_press, button_long_cb_t on_long);
