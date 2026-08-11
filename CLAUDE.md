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
bash tools/build_flasher.sh               # build + assemble the browser flasher into dist/
```

There is no host test suite — `test/` holds only PlatformIO's placeholder README,
and verification is on-device via the serial monitor.

Device settings (bot token, WiFi, proxy, language, timezone) are configured at
runtime through the first-boot captive portal, not at build time — there is no
`.env`, no `tools/gen_secrets.py`, no `src/secrets.h`. They live in NVS namespace
`cfg` (`src/settings.c`) and are read through `settings_get()`. One image flashes
to every device; see `src/provision.c` for the AP + DNS hijack + HTTP portal.

`.clangd` points at `.pio/build/esp32-c6-lcd-1_47/compile_commands.json`, so run
a build once before expecting editor completion on ESP-IDF/LVGL headers.

## Architecture

### Boot path

`app_main` (`main.c`) runs, in order: NVS → `msg_queue_init()` (restores the
persisted queue) → `led_init()` → `display_init()` + `ui_init()` →
`settings_load()` + `ui_set_language()` → `wifi_manager_init()` → **either**
`provision_start()` **or** `wifi_manager_connect_sta()` → SNTP → `sd_log_init()`
→ `pager_start()`.

The fork in the middle is the whole provisioning design:

```
!settings_load()  ||  settings_force_ap()   ->  provision_start()   [never returns]
                              otherwise     ->  connect_sta()
                                                  ok    -> pager
                                                  fail  -> settings_request_ap_and_restart()
```

`led_init()` comes before the network on purpose: a device that restored unread
pages from NVS starts blinking immediately rather than after the link is up.
The language is applied before the fork so even the setup screen speaks the
configured language when a previous partial set exists.

Three FreeRTOS tasks, created in this order from `app_main`:

- **lvgl** (prio 2, `display.c`) — owns `lv_timer_handler()` and the flush
  callback. LVGL is not thread-safe: every `lv_*` call from another task must be
  inside `display_lvgl_lock()`/`display_lvgl_unlock()`. The `ui_*` functions take
  that lock themselves, so callers must *not* already hold it.
- **button** (prio 3, `button.c`) — debounced BOOT key; its callback runs on this
  task, pops the head of the message queue, and posts an ack request. It also
  tracks hold time and fires a second, long-press callback at
  `APP_BUTTON_LONG_HOLD_MS` (see "Re-provisioning gesture").
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

### Runtime settings and provisioning

`settings.c` owns NVS namespace `cfg`; `provision.c` owns the captive portal
that writes it. Together they replaced the old `.env` → `tools/gen_secrets.py`
→ `src/secrets.h` build step, so **one image flashes to every device** and
everything that used to be a `SECRET_*` macro (`bot_token`, `wifi_ssid`,
`wifi_password`, `tz_offset_hours`, `ui_language`, `proxy_*`) is now a field of
`app_settings_t`.

Settings are loaded **once** at boot into a file-static copy and read afterwards
through `settings_get()`, which returns a `const` pointer — no NVS traffic on
the poll or receipt paths, and nothing mutates them at runtime. The only writer
is the portal, and it reboots straight after. That is why callers may cache the
pointer but must not expect a value to change without a restart.

- **Two flags decide the boot, and their order of writing matters.**
  `provisioned` is written *last* in `settings_save()`, after every field and
  after clearing `force_ap`, then committed — it is the single atomic point that
  flips the device from "open the portal" to "try STA". A power cut mid-save
  leaves the set incomplete, and the next boot re-portals rather than trying to
  connect with half a config.
- **`force_ap` is sticky across exactly one reboot.** Set by the long BOOT hold
  or by a failed STA attempt, cleared by `settings_save()`, so submitting the
  form always retries the station path.
- **`settings_load()` returns "complete", not "read something".** It fills the
  struct with whatever individual fields existed even on a false return, so the
  portal form can pre-fill a partial or previous attempt. It also clamps
  `ui_language` (a garbage value would index past the string tables) and
  `tz_offset_hours` back to `APP_DEFAULT_TZ_OFFSET_HOURS`.
- **A failed STA connect does not boot-loop.** `main.c` sets `force_ap` and
  reboots into the portal instead — a pager taken to a new flat is recoverable
  without a serial cable.

The portal itself (`provision_start()`) brings up an **open** SoftAP, paints
`ui_show_provision()` (AP name + `http://192.168.4.1`) on the LCD, starts a DNS
hijack task and an `esp_http_server`, then **blocks forever** so `app_main`
cannot fall through and start the pager on top of it. Notes that look
incidental:

- **The AP is open on purpose.** The form itself carries the secret (the bot
  token); a WPA key here would have to be shown on the 320×172 screen *and*
  typed on the phone before the real secret could be entered.
- **The DNS hijack is what makes the sign-in sheet pop.** `dns_hijack_task`
  answers *every* single-question A query with the AP's own address, so an
  iOS/Android/Windows captive probe resolves to the ESP; the 404 handler then
  302s it to `/`. Without it the URL on the LCD still works by hand. The AP's
  netif is also advertised as the DHCP-offered DNS server
  (`wifi_manager_start_ap()`) — both halves are needed.
- **`parse_and_validate()` deliberately mirrors the old `gen_secrets.py` rules**
  (token must contain `:`, SSID required, TZ in −12…14, SOCKS5 needs host+port,
  RFC 1929 user and password set together or not at all). The portal must not
  be a way to store a half-config the runtime would choke on; on failure it
  re-renders the form with a banner and a 400.
- The page is built into one `realloc`-grown buffer and every pre-filled value
  goes through `html_escape()` — a token or SSID with a quote in it must not
  break out of the attribute.
- `wifi_manager` is split into `init()` (netif + event loop + driver, exactly
  once) and `connect_sta()` / `start_ap()` (mode + config + start), because the
  two paths now share a single boot.

#### Re-provisioning gesture

`button_start()` takes two callbacks. The short one fires on the *down edge* as
before; the long one fires once at `APP_BUTTON_LONG_HOLD_MS` (5 s) and is wired
in `pager_task.c` to `settings_request_ap_and_restart()`. A hold therefore fires
**both** — the short press already went out when the key went down — which is
fine for a gesture that ends in a reboot: the acknowledgement it sends first is
a real "I read this". Do not "fix" that by deferring the short press to release;
the pager must react the instant the key goes down.

### Chat commands

`commands.c` answers `/ping` and `/status` in the chat they came from.
`pager_task` runs `filter_commands()` over every poll batch *before* anything
else sees it and closes the gaps in place, so a command never reaches
`msg_queue`, the screen, `screen_activity()` or the delivery receipt — it is a
question about the device, not a page, and nothing about it should need a
button press to clear.

Only the names listed in `commands_try_handle()` are treated this way; every
other message starting with `/` is paged as usual. Adding a command means
adding a branch there plus one `X(...)` line (both language columns) and its
`#define STR_*` accessor in `ui_strings.h` — `STR_CMD_*` are sent to Telegram,
not drawn, so they may use emoji the pager fonts do not carry (spelled as
escaped bytes, per the receipt convention). Since `STR_*` are function calls
now, a format string like `STR_CMD_STATUS_WIFI_FMT` cannot be pasted next to a
literal label; `build_status()` emits label and format as separate `append()`
calls for exactly that reason.
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

`ui_show_provision()` borrows those same widgets for the setup screen rather
than building a second one: it blanks the header/sender/time, deletes any
leftover scroll animation and rewinds `scroll_y` so the URL cannot crawl out of
view, then paints the two numbered steps into the body label. It runs before
the pager task exists, while the backlight is still lit from `display_init()`
and the sleep countdown has not been armed.

`ui_set_statusf()` is the printf-flavoured `ui_set_status()`. It exists because
`STR_*` are runtime lookups now: a call site can no longer write
`STR_ACK_SENT " " LV_SYMBOL_OK`, and everything that pins an icon onto a status
string formats with `%s` instead.

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
ASCII-only and would render Russian message text as boxes, which is true
whichever language the device is configured for. If regenerating, keep
`--no-compress`: LVGL decodes compressed glyph bitmaps only with
`CONFIG_LV_USE_FONT_COMPRESSED`, which is off.

### Storage

Three independent things live in flash/on card, and they must not be confused
with one another:

- **`msg_store.c` — the unread queue, in NVS namespace `pager`.** Every push
  writes its slot blob *then* the meta (head/count/dropped); every pop writes
  just the meta. Slot-before-meta means a power cut between the two leaves the
  orphaned blob outside the live range, to be overwritten by the next push to
  that index. `msg_store_load()` wipes everything on a magic/version/size
  mismatch rather than trusting a blob from another firmware. It owns every
  `nvs_*` call so `msg_queue.c` stays about the ring and its mutex.
- **`settings.c` — device settings, in NVS namespace `cfg`.** Separate
  namespace on purpose: a queue-format change must not take the WiFi
  credentials with it, and vice versa.
- **`sd_log.c` — one file per paged message on the TF card**, at
  `/sdcard/TelegramPager/<chat_id>/<YYYY-MM-DD>/<HH-MM-SS>.txt`. Entirely
  best-effort: a missing card or a failed write is logged and ignored, never
  blocks the pager and never loses a page from the queue. The card shares SPI2
  with the LCD (only CS/MISO are its own), so the bus is initialised once in
  `display_init()` — which is why MISO is in the bus config even though the
  write-only panel never reads. Dates on the card use the same runtime TZ
  offset the screen does, so the tree matches the `[HH:MM]` on the glass.

### Config surfaces

- `src/app_config.h` — board pins, queue sizes, timeouts, Telegram tuning, the
  long-hold threshold, the provisioning AP name/channel and the fallback TZ.
  Compile-time knobs only: nothing a user configures per device lives here.
- `src/settings.h` / `settings.c` — the runtime, per-device settings (NVS `cfg`).
  Anything a user sets on the portal form belongs here, not in `app_config.h`.
- `src/ui_strings.h` — the both-language string list (see below).
- `sdkconfig.defaults` — console over USB-Serial-JTAG (the board has no UART
  bridge), `LV_COLOR_DEPTH_16` with a manual byte swap in the flush callback (do
  not also enable `LV_COLOR_16_SWAP`), full mbedTLS cert bundle and 16 kB TLS
  input buffer. `sdkconfig.esp32-c6-lcd-1_47` is generated from it.
- `partitions.csv` — custom table; the default single-app layout's 1 MB app
  partition is too small for LVGL + mbedTLS + WiFi + httpd, and NVS has to hold
  the persisted queue *and* the settings. The board has 4 MB flash, not the
  DevKitC's 8 MB, which is why `platformio.ini` overrides
  `board_upload.flash_size`.
- `src/CMakeLists.txt` — globs `src/*.c`, so a new file needs no edit; the
  `REQUIRES` list does, though (`esp_http_server` was added for the portal).

### Browser flasher

`web/index.html` + `tools/build_flasher.sh` build a static WebSerial flasher
(esp-web-tools) into `dist/`; `.github/workflows/flasher.yml` runs the same
script on a `v*` tag and pushes `dist/` to Cloudflare Pages. Both `dist/` and
`web/node_modules/` are build output and gitignored — esp-web-tools is vendored
at build time rather than pulled from a CDN at runtime, so the published page is
a closed set of files.

Two things there are load-bearing, both about flash layout:

- **The offsets are read back from the build, never hardcoded.** Bootloader and
  partition-table offsets come from `sdkconfig.esp32-c6-lcd-1_47`, the app offset
  from decoding `partitions.bin` with the IDF's `gen_esp32part.py`. The oversized
  `nvs` in `partitions.csv` puts factory at **`0x40000`**, not the stock
  `0x10000` — a hardcoded offset here would survive review and brick every board
  that visited the page.
- **The manifest lists three parts, not one merged image.** `esptool merge_bin`
  would fill the gap between the table and the app with `0xff`, which erases the
  `cfg` and `pager` NVS namespaces on every update. Separate parts leave NVS
  alone unless the user ticks *Erase device*.

## UI strings

Every string the firmware itself produces lives in `src/ui_strings.h`; nothing
user-facing belongs in a `.c` file. The header holds a single X-macro list,
`UI_STRING_LIST(X)`, with one `X(NAME, "english", "russian")` line per string.
That list generates *both* the `str_id_t` enum (in the header) and the two
parallel `const char *` tables (in `ui_strings.c`), so the tables cannot drift
apart or out of order — the failure mode the old two-block layout had.

Adding a string is two lines: the `X(...)` entry and a
`#define STR_NAME ui_str(STR_ID_NAME)` accessor. The tables live in
`ui_strings.c`, not the header, so each translation unit does not get its own
copy of both arrays.

The active table is chosen **at runtime** by `ui_set_language()`, called once
from `main.c` right after `settings_load()`. One image therefore speaks either
language, and the choice is a setting on the portal form.

Three conventions, one of them a trap:

- **`STR_*` are function calls, not literals.** They expand to `ui_str(...)`, so
  they can never be string-literal-concatenated with adjacent text or an
  `LV_SYMBOL_*`. Format with `%s` instead — `ui_set_statusf()` exists for the
  screen, and `commands.c` splits label and format into separate `append()`
  calls. This is the one thing that breaks when porting old code forward, and
  it breaks at compile time.
- Receipt texts (`STR_RECEIPT_READ`, `STR_RECEIPT_DELIVERED`) and the emoji in
  the `/status` reply are spelled as escaped byte sequences with the readable
  form in a trailing `/* … */` comment — keep both in sync when editing.
- `LV_SYMBOL_*` icons are passed at the call sites, never baked into the list,
  so the header stays free of an LVGL dependency (`telegram.c` includes it).

Message text and sender names come from Telegram untouched, so the fonts keep
the Cyrillic block whatever the configured language is.

## Proxy

Only SOCKS5 is supported, with optional RFC 1929 auth, and the hostname is
resolved by the proxy rather than locally. It is configured on the portal form
and read from `settings_get()` in `net_conn_open()` — the old `#if
SECRET_PROXY_ENABLED` compile-time branch is gone, so both paths are always
built and an empty proxy user means "no RFC 1929 auth". MTProto is refused by design (it
relays Telegram's client protocol and cannot carry Bot API HTTPS); don't add it
without implementing a full MTProto client.
