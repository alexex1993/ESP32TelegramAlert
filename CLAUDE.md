# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP-IDF firmware for the Waveshare ESP32-C6-LCD-1.47: a Telegram bot pages
messages onto the onboard ST7789 LCD, and the BOOT key sends a "read" receipt
back to the chat. It is a ground-up rewrite of the Arduino/FastBot sketch at
`alexex1993/TelegramPagerESP32`. Built with PlatformIO (`framework = espidf`),
single env `esp32-c6-lcd-1_47`. All application code is in `src/`, compiled as
one ESP-IDF component (`src/CMakeLists.txt` globs `src/*.c`).

## Commands

```bash
pio run -e esp32-c6-lcd-1_47              # build
pio run -t upload -e esp32-c6-lcd-1_47    # build + flash
pio device monitor                        # 115200, exception decoder on
pio run -t clean                          # or -t fullclean to drop managed_components/
bash tools/gen_fonts.sh                   # regenerate src/pager_font_*.c (needs Node)
```

There is no host test suite — `test/` holds only PlatformIO's placeholder README,
and verification is on-device via the serial monitor.

A build requires `.env` (copy from `example.env`). `tools/gen_secrets.py` runs as
a PlatformIO pre-build script and regenerates `src/secrets.h` from it every time;
both files are gitignored and must never be committed or hand-edited. The script
hard-fails on missing keys, an unknown `UI_LANGUAGE`, a half-filled proxy
config, or `PROXY_TYPE=mtproto`.

`.clangd` points at `.pio/build/esp32-c6-lcd-1_47/compile_commands.json`, so run
a build once before expecting editor completion on ESP-IDF/LVGL headers.

## Architecture

Three FreeRTOS tasks, created in this order from `app_main`:

- **lvgl** (prio 2, `display.c`) — owns `lv_timer_handler()` and the flush
  callback. LVGL is not thread-safe: every `lv_*` call from another task must be
  inside `display_lvgl_lock()`/`display_lvgl_unlock()`. The `ui_*` functions take
  that lock themselves, so callers must *not* already hold it.
- **button** (prio 3, `button.c`) — debounced BOOT key; its callback runs on this
  task, pops the head of the message queue, and posts an ack request.
- **pager** (prio 4, `pager_task.c`) — the main loop: drain queued receipts →
  long-poll `getUpdates` → answer and strip chat commands → push what is left →
  repaint. Stack is 10 KB because the TLS handshake and chain verification run
  here.

`msg_queue.c` is the shared state between the button and pager tasks and guards
itself with a mutex. Anything the two tasks pass across (ack requests) goes
through a FreeRTOS queue, not shared globals.

Network stack, bottom up: `net_conn.c` (TCP connect, optionally via
`socks5.c`) → `https_client.c` (raw mbedTLS, not `esp_http_client`) →
`telegram.c` (cJSON) → `pager_task.c`.

### Non-obvious decisions

Most of these are already commented at the site; the point here is that they are
load-bearing and shouldn't be "cleaned up".

- **The abort hook.** `https_request_t.abort_fn` is polled while waiting on the
  socket; `pager_task` returns true from it when an ack is queued, which tears
  down the in-flight 25 s long poll so a receipt isn't stuck behind it.
  `HTTPS_ERR_ABORTED` is a normal outcome, not an error — do not back off on it.
- **Requests are HTTP/1.0 on purpose.** That excludes chunked transfer-encoding,
  so read-until-close is an exact body read and the client needs no chunk parser.
- **Timeout ordering.** `APP_HTTP_SOCKET_TIMEOUT_MS` must stay above
  `APP_LONGPOLL_TIMEOUT_S`, or a quiet poll looks like a dead connection.
- **There is no chat allow-list, on purpose.** Every chat that writes to the bot
  is paged, and each receipt is addressed to the `chat_id` its own message
  carried — which is why `pager_msg_t` and the ack queue both carry one instead
  of using a single configured id. The bot token is the only access control.
  Messages with no chat id are dropped: they could be shown but never answered.
- **The update offset advances past dropped updates too** (non-message updates,
  messages with no chat). A stalled offset makes Telegram replay the same batch
  forever.
- **The message queue drops its oldest entry when full** and counts the drops,
  which the header surfaces next to a warning icon — a dropped message can never
  be acknowledged, so it is not silently discarded.
- **Timestamps come from Telegram's `date` field**, shifted by
  `TZ_OFFSET_HOURS`, so `[HH:MM]` is correct even when SNTP never answered.
- **Text is UTF-8 and truncated on character boundaries** (`copy_utf8` in
  `telegram.c`). `APP_MSG_TEXT_MAX` is bytes; Cyrillic is 2 bytes/char. This,
  not the body scroll, is what caps how much of a long message is readable — a
  cut here looks on screen exactly like a scroll that stops early. A
  `pager_msg_t` is big enough that neither the queue slots nor `pager_task`'s
  poll batch may live on a stack; the batch is `static` for that reason, and
  the button task's stack is sized for the two that the press path holds live
  at once.

### Chat commands

`commands.c` answers `/ping` and `/status` in the chat they came from.
`pager_task` runs `filter_commands()` over every poll batch *before* anything
else sees it and closes the gaps in place, so a command never reaches
`msg_queue`, the screen, `screen_activity()` or the delivery receipt — it is a
question about the device, not a page, and nothing about it should need a
button press to clear.

Only the names listed in `commands_try_handle()` are treated this way; every
other message starting with `/` is paged as usual. Adding a command means
adding a branch there and its strings to *both* blocks of `ui_strings.h` —
`STR_CMD_*` are sent to Telegram, not drawn, so they may use emoji the pager
fonts do not carry (spelled as escaped bytes, per the receipt convention).
The technical tokens in the reply (`SOCKS5`, the `esp_reset_reason()` names)
are deliberately untranslated: they read like `esp_err_to_name()` output.

Two things that look incidental but are not: the reply buffer is `static` for
the same reason the poll batch is (the TLS handshake runs on the pager stack
while it is live), and `pager_task` skips its `idle_status()` call on a lap
that answered a command — the poll returns the instant one arrives, so the
idle text would wipe the footer feedback before it could be read.

The status reply reports *whether* a proxy is in use, never which one. There
is no allow-list on commands, matching the messages: the token is the access
control.

### Display and fonts

The 172×320 glass is driven rotated to 320×172 by the ST7789 itself (MADCTL
swap + one mirror), not by LVGL software rotation. That is why
`BOARD_LCD_GAP_Y` is 34 rather than a gap on x — the swap moves the panel's
240-column RAM offset onto y. Pins and the mirror flags live in `app_config.h`;
set `BOARD_LCD_SELFTEST` to 1 there to separate "panel addressing wrong" from
"fault above the driver".

The message body is a scrollable container (`s_body_view`) wrapping the label,
not a bare label, because a message taller than the ~100 px body area would
otherwise be clipped with no way to reach the rest — the BOOT key is the
acknowledge button and there is no touch. `body_restart_scroll()` in `ui.c`
measures the overflow after the text changes and, if there is any, animates
`scroll_y` on a loop: pause, linear crawl down, pause, quick rewind. It is
restarted only when the *head of the queue* changes, keyed on
(`chat_id`, `message_id`); `ui_render_queue()` also runs for repaints that leave
the head alone, and restarting on those would jerk the text back mid-read.
Because of that short-circuit, `ui_init` has to paint the empty-queue state
itself. Speed and dwell live in `app_config.h` as `UI_BODY_SCROLL_*`.

### Screen sleep

`screen.c` holds the backlight policy: the glass lights **only on a BOOT key
press**, never on a message arriving. An arriving message is signalled by the
RGB LED (see "RGB LED" below) — the screen comes up when the pager is picked up
and the key is hit. While open it stays lit as long as the queue still holds
something to acknowledge, then dims after a `UI_SCREEN_ON_MS` (20 s) grace so
the last receipt can be read. Only the backlight is switched — the panel, LVGL
and the poll loop all keep running, so waking shows the current screen with
nothing to redraw. `display.c` exposes `display_set_backlight()` and knows
nothing about when it should be called.

`screen_activity()` ("a key was pressed") lights the screen and either holds
it (queue non-empty) or arms the one-shot off timer. `pager_task.c` calls it
from the button path only — arrivals deliberately do not — and once at task
start, via `screen_arm_off()`, to begin the boot countdown regardless of any
restored pages. The off timer's callback dims unconditionally now: with
arrivals no longer holding the glass lit, nothing should override the
countdown, and a message that lands during the grace window is the LED's job
to advertise, not the screen's. Its mutex still serialises that callback
against a concurrent `screen_activity()`.

Because the screen can be dark while the queue is non-empty, the
`screen_is_on()` guard in `on_button_press` regularly suppresses what would
otherwise be an immediate acknowledgement: the press that wakes a pager with
waiting pages only lights it, and the next press acknowledges. That is
intentional — the first press is "I'm looking", not "I'm done".

The body-scroll need not be paused while the screen sleeps: it loops forever
by design ("the pager is glanced at, not watched"), so a message that arrived
in the dark is simply mid-cycle the moment the glass lights, which is exactly
how one picked up mid-read behaves.

### RGB LED

The on-board WS2812 (`led.c`, GPIO8 per the Waveshare wiki) is the unread cue
now that the screen no longer lights on arrival. A low-priority task polls
`msg_queue_count()` and blinks the single pixel while it is above zero, dark
otherwise — polling rather than a push/pop feed so a missed or future call
site can never leave it stuck on or stuck off. Cadence and colour live in
`app_config.h` (`BOARD_LED_BLINK_*_MS`, `BOARD_LED_COLOR_*`); the colour is
deliberately dim, since one WS2812 driven wide open is harsh behind the
diffuser.

`src/pager_font_14.c` / `pager_font_20.c` are generated and **committed** —
normal builds do not regenerate them. LVGL's stock Montserrat fonts are
ASCII-only and would render Russian message text as boxes, which is true no
matter which `UI_LANGUAGE` the firmware was built with. If regenerating, keep
`--no-compress`: LVGL decodes compressed glyph bitmaps only with
`CONFIG_LV_USE_FONT_COMPRESSED`, which is off.

### Config surfaces

- `src/app_config.h` — board pins, queue sizes, timeouts, Telegram tuning.
- `src/ui_strings.h` — the per-language string tables (see below).
- `sdkconfig.defaults` — console over USB-Serial-JTAG (the board has no UART
  bridge), `LV_COLOR_DEPTH_16` with a manual byte swap in the flush callback (do
  not also enable `LV_COLOR_16_SWAP`), full mbedTLS cert bundle and 16 kB TLS
  input buffer. `sdkconfig.esp32-c6-lcd-1_47` is generated from it.
- `partitions.csv` — custom table; the default single-app layout's 1 MB app
  partition is too small for LVGL + mbedTLS + WiFi. The board has 4 MB flash,
  not the DevKitC's 8 MB, which is why `platformio.ini` overrides
  `board_upload.flash_size`.

## UI strings

Every string the firmware itself produces lives in `src/ui_strings.h` as one
`STR_*` macro per language block; nothing user-facing belongs in a `.c` file.
`UI_LANGUAGE` in `.env` (`english`, the default, or `russian`) picks the block
at compile time via `SECRET_UI_LANGUAGE_ID`, so only one table is ever built —
there is no runtime language switch and no lookup table. Adding a string means
adding it to *both* blocks; a missing one is a compile error, which is the point.

Two conventions inside that header:

- Receipt texts (`STR_RECEIPT_READ`, `STR_RECEIPT_DELIVERED`) are spelled as
  escaped byte sequences with the readable form in a trailing comment — keep
  both in sync when editing.
- `LV_SYMBOL_*` icons are concatenated at the call sites, not baked into the
  macros, so the header stays free of an LVGL dependency (`telegram.c` includes
  it).

Message text and sender names come from Telegram untouched, so the fonts keep
the Cyrillic block whatever `UI_LANGUAGE` says.

## Proxy

Only SOCKS5 is supported, with optional RFC 1929 auth, and the hostname is
resolved by the proxy rather than locally. MTProto is refused by design (it
relays Telegram's client protocol and cannot carry Bot API HTTPS); don't add it
without implementing a full MTProto client.
