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

// ---- SD card (TF) -------------------------------------------------------
// The onboard TF slot shares the LCD's SPI2 bus: MOSI/SCLK are the same pins
// (GPIO6/7), only CS (GPIO4) and MISO (GPIO5) are SD-specific. The bus is
// initialised once in display_init(), so MISO must be wired into the bus
// config there even though the write-only LCD never reads -- the SDSPI device
// reads through it. See sd_log.c for the mount and per-message file writes.
#define BOARD_SD_SPI_HOST    SPI2_HOST
#define BOARD_SD_PIN_CS      4
#define BOARD_SD_PIN_MISO    5
#define BOARD_SD_PIN_MOSI    BOARD_LCD_PIN_MOSI
#define BOARD_SD_PIN_SCLK    BOARD_LCD_PIN_SCLK
// The card probes at this cap; the driver picks the highest rate the card
// advertises. 20 MHz is the documented default-speed ceiling and is well clear
// of the 40 MHz LCD clock on the shared bus.
#define BOARD_SD_FREQ_KHZ    SDMMC_FREQ_DEFAULT
#define BOARD_SD_MOUNT_POINT "/sdcard"
// Root of the message log tree on the card.
#define SD_LOG_ROOT          BOARD_SD_MOUNT_POINT "/TelegramPager"

// ---- Button -------------------------------------------------------------
// The board's BOOT key. It is a strapping pin (holding it through reset
// enters download mode), but reading it at runtime is fine. Active low, with
// the internal pull-up enabled.
#define BOARD_BUTTON_GPIO        9
#define BOARD_BUTTON_ACTIVE_LEVEL 0
#define BOARD_BUTTON_DEBOUNCE_MS 40
// Holding BOOT this long triggers the re-provisioning gesture (long-press
// callback in button.c). 5 s is long enough that a normal "acknowledge" press
// never trips it, short enough to be usable.
#define APP_BUTTON_LONG_HOLD_MS  5000

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
// The slots are a static array, so this costs sizeof(pager_msg_t) bytes of
// DRAM per slot whether or not anything is in it, and that DRAM comes straight
// off the heap. Measured, linear, back when a slot was 2120 bytes: 8 slots left
// 203 kB of heap, 32 left 177 kB, 64 left 143 kB, 128 left 75 kB. A slot is
// 2248 bytes since inline_message_id joined the struct, so read those figures
// as roughly 4 kB lower at 32 slots and 8 kB at 64. The ceiling is not the
// static budget but the runtime peak -- WiFi, the LVGL draw buffers and above
// all the TLS handshake against the full certificate bundle, which together
// are the same order as what 128 slots would leave. That failure would not
// show up at boot but on the first getUpdates, so do not raise this past 64
// without checking the "min" figure that /status reports once the device has
// been up long enough to have shaken hands at least once.
#define APP_MSG_QUEUE_LEN     32
// Bytes, not characters -- Cyrillic is two bytes per character in UTF-8, so
// this holds roughly 1000 Russian characters. Longer messages are truncated on
// a character boundary.
//
// This is what actually bounds how much of a long message can be read: the
// body scrolls, but only over text that survived this cut. Raising it is not
// free -- a pager_msg_t sits in every queue slot and in the poll batch, so the
// cost is APP_MSG_QUEUE_LEN + APP_UPDATES_PER_POLL times any increase, and
// neither of those buffers may go back onto a task stack.
#define APP_MSG_TEXT_MAX      2048
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
// After the queue empties the glass dims on a three-phase schedule -- full,
// then dim, then a smooth fade to off -- long enough to see the receipt go
// out, short enough that a pager left on a desk goes dark. A BOOT key press
// at any moment snaps it back to full and restarts the chain (and the wake
// press that lights a dark screen is swallowed rather than acknowledging
// anything).
//
//   FULL   for UI_SCREEN_FULL_MS  at UI_SCREEN_FULL_LEVEL,
//   DIM    for UI_SCREEN_DIM_MS   at UI_SCREEN_DIM_LEVEL,
//   FADING over UI_SCREEN_FADE_MS down to 0.
//
// Levels are 0..1000 (per-mille of DISPLAY_BL_MAX) so the fade math divides
// evenly into UI_SCREEN_FADE_MS without a fractional accumulator.
#define UI_SCREEN_FULL_MS           20000
#define UI_SCREEN_FULL_LEVEL        1000
#define UI_SCREEN_DIM_MS            10000
#define UI_SCREEN_DIM_LEVEL         500   // 50%
#define UI_SCREEN_FADE_MS           2000

// ---- Telegram Bot API ---------------------------------------------------
#define TELEGRAM_API_HOST     "api.telegram.org"
#define TELEGRAM_API_PORT     443
// Seconds the server holds a getUpdates request open when there is nothing to
// report. Kept below the socket timeout below.
#define APP_LONGPOLL_TIMEOUT_S   25
#define APP_UPDATES_PER_POLL     5
// Mark each incoming message the moment it lands, like the original pager
// did. The button-triggered "read" mark is separate and always sent.
//
// A mark is a reaction now, not a reply: setMessageReaction leaves one emoji
// on the message instead of adding a second message to the chat. A bot may
// hold exactly one reaction per message, so the "read" emoji replaces the
// "delivered" one, which is the progression we want. Chats can forbid
// reactions outright (and channels forbid them for bots), so pager_task falls
// back to the old STR_RECEIPT_* text reply whenever the reaction is refused.
#define APP_SEND_DELIVERY_RECEIPT 1
#define APP_ACK_MAX_ATTEMPTS      3

// ---- Chat commands ------------------------------------------------------
// "/last N" reads the N newest unread pages back out into the chat that asked.
// Three numbers bound the reply, and the first two exist because of a hard
// Telegram limit rather than taste: sendMessage refuses anything over 4096
// characters, and a page here may be APP_MSG_TEXT_MAX bytes on its own. Each
// page is therefore cut (on a character boundary, with an ellipsis) at
// APP_LAST_TEXT_MAX and no more than APP_LAST_MAX_PAGES of them go into one
// reply -- COMMANDS_LAST_MAX in commands.h is derived from the two and
// static-asserted against that limit.
//
// Raising either costs DRAM twice over: the reply buffer grows with the
// product, and it is static for the same reason the poll batch is (the TLS
// handshake that sends it runs on the pager task's stack).
#define APP_LAST_MAX_PAGES       8
#define APP_LAST_DEFAULT_PAGES   3   // A bare "/last", with no number after it.
// Bytes, not characters, like APP_MSG_TEXT_MAX -- so roughly 160 Cyrillic
// characters of each page. A listing is a reminder of what is waiting, not a
// way to read it: the full text is on the glass.
#define APP_LAST_TEXT_MAX        320

// ---- Inline mode --------------------------------------------------------
// "@thisbot ..." typed in any chat, without the bot being a member of it:
// an empty query offers the device's status, a non-empty one offers to page
// it (see inline_mode.c).
//
// Two things the firmware cannot switch on by itself -- both are BotFather
// settings, and there is no Bot API method for either:
//   /setinline         -- turns the mode on at all, and sets the placeholder.
//   /setinlinefeedback -- must be Enabled, or the chosen_inline_result update
//                         never arrives and a picked page is posted in the
//                         chat but never reaches the device.
//
// Telegram fires a fresh inline_query on every keystroke and this device pays
// a full TLS handshake per answer, so only the newest query per user in a
// batch is answered -- see dedupe_queries() in pager_task.c.
#define APP_INLINE_QUERIES_PER_POLL   APP_UPDATES_PER_POLL
#define APP_INLINE_CALLBACKS_PER_POLL APP_UPDATES_PER_POLL
// Telegram caps an inline query at 256 characters and Cyrillic costs two
// bytes each. This also bounds the text of an inline page, which is why it is
// far below APP_MSG_TEXT_MAX -- the whole body has to be resent on every edit.
#define APP_INLINE_QUERY_MAX     520
// inline_query.id and callback_query.id are decimal strings of a 64-bit value.
#define APP_INLINE_ID_MAX        32
// inline_message_id is an opaque token, not a number; observed lengths sit
// well under this. Too short would cost the receipt, not the page.
#define APP_INLINE_MSG_ID_MAX    128
// Seconds Telegram may cache an answer. Zero: the status card has to be
// current, and the page card differs on every keystroke anyway.
#define APP_INLINE_CACHE_TIME_S  0

// ---- Contacts and the boot announcement ---------------------------------
// Every chat that writes to the pager is remembered in NVS (namespace
// "contacts", see contacts.c), and each one is told once, in turn, that the
// device is back up -- so a pager that lost power does not leave the people
// paging it wondering whether it is still listening.
//
// The whole list is a single NVS blob of this many entries, and it is static
// DRAM as well, at sizeof(contact_t) (64 bytes) each -- so this is cheap, but
// it is also the number of TLS handshakes a boot can cost before the first
// poll: at roughly a second each, 16 is a few seconds of announcing. A full
// list replaces its least recently seen entry.
#define APP_CONTACTS_MAX          16
// How stale a contact's last-seen stamp is allowed to get before writing a
// fresher one. The stamp only orders the eviction above, so refreshing it on
// every single message would spend a flash write to sharpen a number nothing
// reads that precisely.
#define APP_CONTACTS_TOUCH_S      3600
// Announce at all. Turning this off keeps the list (it is what /status counts
// and what a future feature would reuse) but leaves the chats quiet on boot.
#define APP_ANNOUNCE_ON_BOOT      1

// ---- Networking ---------------------------------------------------------
// Retries per configured network (see app_settings_t): wifi_manager walks the
// up-to-three slots in order and gives each this many attempts before moving
// to the next; only when every slot is exhausted does the boot fall back to
// the provisioning portal.
#define APP_WIFI_MAX_RETRY       10
// ---- WiFi runtime monitor / failover --------------------------------------
// After the boot connect succeeds, main.c arms a monitor task in wifi_manager
// (wifi_manager_start_monitor). While the link is up it background-scans every
// APP_WIFI_MONITOR_PERIOD_MS (a sweep costs ~2 s of airtime, which TCP absorbs
// -- the long poll stalls briefly under its socket timeout and resumes), and
// when the link drops it scans, ranks the configured networks by RSSI and
// reconnects to the best visible one. It retries forever with backoff rather
// than rebooting into the portal: the queue, the LED and the screen keep
// working offline, and re-provisioning stays a long BOOT hold away.
//
// Wait per candidate during failover. Well above a WPA handshake; wrong
// credentials usually fail faster than this with an AUTH error.
#define APP_WIFI_CONNECT_WAIT_MS        12000
// Idle period between proactive scans while connected.
#define APP_WIFI_MONITOR_PERIOD_MS      60000
// Proactive roam: while connected, if the current AP sinks below this RSSI
// (dBm) and another configured network beats it by APP_WIFI_ROAM_MARGIN_DB,
// switch before the link dies on its own. The margin and the cooldown below
// are the anti-flap: two weak-but-usable networks must not ping-pong the
// radio between them.
#define APP_WIFI_ROAM_RSSI_THRESHOLD_DBM  (-85)
#define APP_WIFI_ROAM_MARGIN_DB           12
#define APP_WIFI_ROAM_COOLDOWN_MS         60000
// Retry cadence once every visible candidate has failed: doubles up to the max.
#define APP_WIFI_FAILOVER_BACKOFF_MIN_MS  10000
#define APP_WIFI_FAILOVER_BACKOFF_MAX_MS  60000
// Per-socket-read timeout. Must exceed APP_LONGPOLL_TIMEOUT_S so a quiet long
// poll is not mistaken for a dead connection.
#define APP_HTTP_SOCKET_TIMEOUT_MS  (APP_LONGPOLL_TIMEOUT_S * 1000 + 15000)
#define APP_HTTP_CONNECT_TIMEOUT_MS 10000
#define APP_HTTP_MAX_RESPONSE       (32 * 1024)
// Backoff after a failed poll, so a broken proxy does not spin the radio.
#define APP_POLL_ERROR_BACKOFF_MS   5000

// ---- Provisioning (first-boot captive portal) ---------------------------
// On first boot, or after the long-press re-provision gesture, the device
// brings up an open SoftAP with this SSID and serves a one-page web form at
// its own address (the ESP-IDF default 192.168.4.1 -- the captive-portal
// standard, so phones pop the sign-in sheet automatically). The SSID is open
// on purpose: the form itself carries the secret (bot token), and a WPA key
// here would have to be re-entered on the phone and shown on the screen too.
#define APP_PROVISION_AP_SSID      "TelegramPager"
#define APP_PROVISION_AP_CHANNEL   6

// Default TZ offset applied when a freshly-provisioned set somehow lacks one
// (should not happen -- the form always submits a value -- but settings_load
// clamps a garbage value back to this rather than render garbage times).
#define APP_DEFAULT_TZ_OFFSET_HOURS 3
