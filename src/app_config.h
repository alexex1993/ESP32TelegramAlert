#pragma once

// ---- Board: Waveshare ESP32-C6-LCD-1.47 (ST7789 on SPI2) ----------------
#define BOARD_LCD_SPI_HOST    SPI2_HOST
#define BOARD_LCD_PIN_MOSI    6
#define BOARD_LCD_PIN_SCLK    7
#define BOARD_LCD_PIN_CS      14
#define BOARD_LCD_PIN_DC      15
#define BOARD_LCD_PIN_RST     21
#define BOARD_LCD_PIN_BL      22
#define BOARD_LCD_BL_ON_LEVEL 1

// The glass is 172x320 portrait, but a pager is read along the long edge, so
// the panel is driven in landscape and LVGL sees 320x172. The rotation is
// done by the ST7789 itself (MADCTL row/column exchange) rather than by
// LVGL's software rotation, which would cost a full-frame CPU copy.
#define BOARD_LCD_H_RES       320
#define BOARD_LCD_V_RES       172

// The controller has 240 columns of RAM and this 172px-wide glass sits
// centred in it, so addresses on that axis need +34. Swapping x/y moves that
// axis from x to y, hence the gap on y rather than x.
#define BOARD_LCD_SWAP_XY     true
#define BOARD_LCD_GAP_X       0
#define BOARD_LCD_GAP_Y       34
// Swapping x/y alone transposes the image; exactly one mirror turns that into
// a true 90-degree rotation. Flip these two if the display comes out upside
// down or mirrored on your unit.
#define BOARD_LCD_MIRROR_X    true
#define BOARD_LCD_MIRROR_Y    false

// Diagnostic, off by default. Set to 1 to paint the screen red/green/blue
// straight through esp_lcd and then magenta through LVGL before the UI comes
// up, and to log the first flushes. That splits a blank screen into "panel
// addressing wrong" (nothing shows) versus "fault above the driver" (the
// colours show but the UI does not).
#define BOARD_LCD_SELFTEST    0

#define BOARD_LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS    8
#define BOARD_LCD_PARAM_BITS  8

// ---- Button -------------------------------------------------------------
// The board's BOOT key. It is a strapping pin (holding it through reset
// enters download mode), but reading it at runtime is fine. Active low, with
// the internal pull-up enabled.
#define BOARD_BUTTON_GPIO        9
#define BOARD_BUTTON_ACTIVE_LEVEL 0
#define BOARD_BUTTON_DEBOUNCE_MS 40

// ---- Message queue ------------------------------------------------------
// Unacknowledged messages waiting to be read. When full the oldest is
// dropped: on a pager the newest page is the one that matters, and a dropped
// message can no longer be acknowledged, so the count of drops is surfaced
// on screen.
#define APP_MSG_QUEUE_LEN     8
// Bytes, not characters -- Cyrillic is two bytes per character in UTF-8, so
// this holds roughly 500 Russian characters. Longer messages are truncated on
// a character boundary.
//
// This is what actually bounds how much of a long message can be read: the
// body scrolls, but only over text that survived this cut. Raising it is not
// free -- a pager_msg_t sits in every queue slot and in the poll batch, so the
// cost is APP_MSG_QUEUE_LEN + APP_UPDATES_PER_POLL times any increase, and
// neither of those buffers may go back onto a task stack.
#define APP_MSG_TEXT_MAX      1024
#define APP_MSG_FROM_MAX      48

// ---- UI -----------------------------------------------------------------
// A message taller than the body area scrolls itself, hands-off: pause at the
// top, creep down at this speed, pause at the bottom, glide back up, repeat.
// The BOOT key is the acknowledge button and nothing else, so self-scrolling
// is the only way a long page can be read in full.
// Pixels per second of the downward crawl. The body font is 20px tall, so this
// is roughly one line every 1.3 s.
#define UI_BODY_SCROLL_SPEED_PX_S   18
// Dwell at the top before starting, and at the bottom before rewinding.
#define UI_BODY_SCROLL_PAUSE_MS     1800
// The rewind is a single quick glide, not a re-read, so it is a fixed time
// rather than a speed.
#define UI_BODY_SCROLL_RETURN_MS    500

// ---- Telegram Bot API ---------------------------------------------------
#define TELEGRAM_API_HOST     "api.telegram.org"
#define TELEGRAM_API_PORT     443
// Seconds the server holds a getUpdates request open when there is nothing to
// report. Kept below the socket timeout below.
#define APP_LONGPOLL_TIMEOUT_S   25
#define APP_UPDATES_PER_POLL     5
// Reply to each incoming message the moment it lands, like the original
// pager did. The button-triggered "read" receipt is separate and always sent.
#define APP_SEND_DELIVERY_RECEIPT 1
#define APP_ACK_MAX_ATTEMPTS      3

// ---- Networking ---------------------------------------------------------
#define APP_WIFI_MAX_RETRY       10
// Per-socket-read timeout. Must exceed APP_LONGPOLL_TIMEOUT_S so a quiet long
// poll is not mistaken for a dead connection.
#define APP_HTTP_SOCKET_TIMEOUT_MS  (APP_LONGPOLL_TIMEOUT_S * 1000 + 15000)
#define APP_HTTP_CONNECT_TIMEOUT_MS 10000
#define APP_HTTP_MAX_RESPONSE       (32 * 1024)
// Backoff after a failed poll, so a broken proxy does not spin the radio.
#define APP_POLL_ERROR_BACKOFF_MS   5000
