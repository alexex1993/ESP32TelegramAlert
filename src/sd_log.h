#pragma once

#include <stdbool.h>

#include "msg_queue.h"

// Mounts the onboard TF card (shares SPI2 with the LCD) at BOARD_SD_MOUNT_POINT
// and prepares the log tree. Best-effort: on a missing card or mount failure it
// returns false and the pager keeps running without logging -- nothing here may
// block or restart the device. Idempotent; safe to call once after the SPI bus
// (display_init) is up.
bool sd_log_init(void);

// True when the card is mounted and sd_log_message() will actually write.
bool sd_log_mounted(void);

// Writes one paged message to its own file under
//   <mount>/TelegramPager/<chat_id>/<YYYY-MM-DD>/<HH-MM-SS>.txt
// using the message's own Telegram date shifted by SECRET_TZ_OFFSET_HOURS, so
// the on-card tree matches the [HH:MM] the screen shows. Best-effort: a write
// failure is logged but never blocks the pager or loses a page.
void sd_log_message(const pager_msg_t *msg);
