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

// ---- RGB LED (unread indicator) -----------------------------------------
// The board's on-board WS2812 -- a single addressable pixel behind the acrylic
// diffuser. Per the Waveshare ESP32-C6-LCD-1.47 wiki, RGB_Control is wired to
// GPIO8. It is the only arrival cue now that the screen lights on a key press
// rather than on a message: it blinks while the queue holds anything unread
// and is dark otherwise (see led.c).
#define BOARD_LED_GPIO    8
#define BOARD_LED_COUNT   1
// Pulse cadence: a short flash then a longer gap, roughly once a second.
// Visible from across a room without strobing on a desk.
#define BOARD_LED_BLINK_ON_MS   150
#define BOARD_LED_BLINK_OFF_MS  850
// How often the blink task re-checks the queue while idle. Cheap, so short is
// fine: one mutex take and a compare.
#define BOARD_LED_IDLE_POLL_MS  250
// Colour, 0..255 per channel. Kept well under 255: one WS2812 driven wide open
// is harsh and the diffuser makes it worse, so this is a "something to read"
// nudge rather than a beacon. Green by default.
#define BOARD_LED_COLOR_R   0
#define BOARD_LED_COLOR_G   45
#define BOARD_LED_COLOR_B   0

// ---- Message queue ------------------------------------------------------
// Unacknowledged messages waiting to be read. When full the oldest is
// dropped: on a pager the newest page is the one that matters, and a dropped
// message can no longer be acknowledged, so the count of drops is surfaced
// on screen.
//
// The queue is persisted in NVS (see msg_store.c): every push and pop writes
// through to flash, so a power cut does not lose unread pages. NVS holds one
// ~1KB blob per slot plus a small meta blob under the "pager" namespace,
// which is why the nvs partition in partitions.csv is sized for
// APP_MSG_QUEUE_LEN * sizeof(pager_msg_t) plus headroom (currently 64KB).
//
// The slots are a static array, so this costs sizeof(pager_msg_t) = 1096 bytes
// of DRAM per slot whether or not anything is in it, and that DRAM comes
// straight off the heap. Measured, linear: 8 slots leave 203 kB of heap, 32
// leave 177 kB, 64 leave 143 kB, 128 leave 75 kB. The ceiling is not the
// static budget but the runtime peak -- WiFi, the LVGL draw buffers and above
// all the TLS handshake against the full certificate bundle, which together
// are the same order as what 128 slots would leave. That failure would not
// show up at boot but on the first getUpdates, so do not raise this past 64
// without checking the "min" figure that /status reports once the device has
// been up long enough to have shaken hands at least once.
#define APP_MSG_QUEUE_LEN     32
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

// Sleep: the backlight is lit only while a message is waiting to be read.
// This is the grace window it stays up for after the queue empties -- long
// enough to see the receipt go out, short enough that a pager left on a desk
// is dark. A BOOT key press restarts it (and that press is swallowed rather
// than acknowledging anything).
#define UI_SCREEN_ON_MS             20000

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
