#pragma once

// WS2812 unread indicator. The single addressable RGB LED on GPIO8 (per the
// Waveshare wiki) is the only arrival cue now that the screen lights on a key
// press rather than on a message: it blinks while the queue holds anything
// unread and stays dark otherwise.
//
// It is decoupled from the screen -- it keeps blinking even while a message is
// open, until that message is acknowledged and the queue drains.

// Creates the LED driver and starts the blink task. Call after
// msg_queue_init(); the task polls msg_queue_count() to decide whether to
// blink, so it needs the queue ready.
void led_init(void);
