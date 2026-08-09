#pragma once

typedef void (*button_press_cb_t)(void);

// Starts a task that watches the BOOT key and calls `on_press` once per
// press, from that task's context (so the callback may block briefly and may
// take the LVGL lock).
void button_start(button_press_cb_t on_press);
