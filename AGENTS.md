# AGENTS.md

Compact guide for OpenCode sessions. The deep architectural commentary
lives in `CLAUDE.md` — read it before changing the pager, button, screen,
HTTPS, or UI code. This file is the short version: what to run, what to
verify, and what looks wrong but is not.

## Build / flash / observe

```bash
pio run -e esp32-c6-lcd-1_47              # build
pio run -t upload -e esp32-c6-lcd-1_47    # build + flash
pio device monitor                        # 115200, exception decoder on
pio run -t fullclean                      # also drops managed_components/
```

Single env, single ESP-IDF component. `src/CMakeLists.txt` globs `src/*.c`,
so a new `.c` file is picked up with no CMakeLists edit — but a `.c` placed
anywhere other than `src/` is invisible to the build.

## What there is *not*

- **No host test suite, no lint, no typecheck.** `test/`, `lib/`, `include/`
  hold only PlatformIO placeholders. Verification is on-device via the serial
  monitor — do not invent a `npm test` / `pytest` / `ruff` step. After a code
  change, `pio run` is the typecheck.
- **No `.env`, no `secrets.h`, no allow-list, no pinned TLS cert.** Device
  settings live in NVS and are configured through the captive portal. See
  `CLAUDE.md` for the reasoning; do not add any of those back.

## Device settings and codegen

- **Settings are runtime, not build-time.** They live in NVS namespace `cfg`
  (`src/settings.c`) and are edited through the first-boot captive portal
  (`src/provision.c`): open AP `TelegramPager`, phone pops the form, submit
  reboots into station mode. No `.env`, no `tools/gen_secrets.py`, no
  `src/secrets.h` — one image flashes to every device. Everything that used to
  read a `SECRET_*` macro now reads `settings_get()`.
- **`settings_get()` is a `const` pointer to a copy loaded once at boot.** No
  NVS traffic on the poll/receipt paths, and no value changes without a reboot
  — the portal is the only writer and it restarts the device right after.
- The boot fork lives in `main.c`:

  ```
  !settings_load() || settings_force_ap()  -> provision_start()   [never returns]
                             otherwise     -> wifi_manager_connect_sta()
                                                ok   -> sd_log_init(), pager_start()
                                                fail -> settings_request_ap_and_restart()
  ```

  `force_ap` is set by a **5 s BOOT hold** (wired in `pager_task.c`) or by a
  STA-connect failure, and cleared by `settings_save()`, so it survives exactly
  the one reboot that routes into the portal.
- **`settings_load()` returns "complete", not "read something"** — on a false
  return the struct still holds whatever fields existed, which is what pre-fills
  the form. It clamps `ui_language` (a bad value would index past the string
  tables) and an out-of-range `tz_offset_hours`.
- **`provisioned` is written last in `settings_save()`**, after the fields and
  after clearing `force_ap`. That single commit is what flips the device from
  "portal" to "STA"; a power cut before it re-portals instead of connecting
  with half a config.
- `src/pager_font_14.c` / `pager_font_20.c` are generated **and committed**.
  Normal builds do **not** regenerate them. Only `bash tools/gen_fonts.sh`
  does — it needs Node (npx) **and** `managed_components/lvgl__lvgl/` present,
  so run a build once before regenerating. The `--no-compress` flag is
  mandatory: `CONFIG_LV_USE_FONT_COMPRESSED` is off, so compressed glyphs
  decode to garbage.

## The provisioning portal (`provision.c`)

`provision_start()` = open SoftAP → `ui_show_provision()` (SSID + URL on the
LCD) → DNS-hijack task → `esp_http_server` on `/` and `/save` → **block
forever** so `app_main` cannot fall through and start the pager on top of it.

- **The AP is open on purpose** — the form carries the real secret (the bot
  token); a WPA key would have to be shown on the glass *and* typed on the phone
  first. Don't "harden" it.
- **The sign-in popup needs both halves**: `wifi_manager_start_ap()` advertises
  the AP's own address as the DHCP DNS server, and `dns_hijack_task` answers
  every single-question A query with that address. The 404 handler then 302s
  the captive probe to `/`. Remove either half and the popup stops appearing —
  the URL on the LCD still works by hand, which is why this fails quietly.
- **`parse_and_validate()` mirrors the old `gen_secrets.py` rules** (token
  contains `:`, SSID present, TZ −12…14, SOCKS5 needs host+port, RFC 1929
  user/pass both or neither). Keep them: the portal must not be able to store a
  config the runtime chokes on. On failure it re-renders the form with a banner
  and a 400.
- Pre-filled values go through `html_escape()`; the page is built into one
  `realloc`-grown buffer and freed by the handler.
- `esp_http_server` is in `src/CMakeLists.txt`'s `REQUIRES` — the glob picks up
  new `.c` files, but a new IDF component dependency still needs that edit.

`wifi_manager` is split accordingly: `wifi_manager_init()` (netif + event loop +
driver, **exactly once**, from `app_main`), then `connect_sta()` *or*
`start_ap()`. `wifi_manager_get_ap_ip()` is what the on-screen URL is built
from, so a changed AP address needs no edit in `provision.c`.

`button_start()` now takes **two** callbacks: short (down edge, as before) and
long (`APP_BUTTON_LONG_HOLD_MS`, 5 s). A long hold fires **both** — the short
one already went out when the key went down. That is accepted, not a bug: the
gesture ends in a reboot and the acknowledgement it sent first was genuine. Do
not move the short press to release to "fix" it.

## Editor / clangd

`.clangd` points at `.pio/build/esp32-c6-lcd-1_47/compile_commands.json`.
Run `pio run` once before expecting completion on ESP-IDF/LVGL headers;
after `fullclean` completion is broken until the next build regenerates it.

## Config surfaces (where a change actually goes)

| File | Holds |
| --- | --- |
| `src/app_config.h` | board pins, queue sizes, timeouts, Telegram tuning, provisioning AP name |
| `src/ui_strings.h` | both-language string tables (X-macro), resolved at runtime (see below) |
| `src/settings.c` | runtime device settings in NVS (cfg): token, wifi, tz, language, proxy |
| `sdkconfig.defaults` | console USB-Serial-JTAG, RGB565 + manual byte swap, full mbedTLS bundle, 16 kB TLS in-buffer, 4 MB flash, and the size block: `-Os`, the explicit `LV_USE_*` widget list, LVGL on the system heap, two blend destinations, client-only TLS, no IPv6 |
| `partitions.csv` | custom table (LVGL+mbedTLS+WiFi+httpd > the default 1 MB app slot); NVS for the persisted queue + settings |
| `platformio.ini` | one env `esp32-c6-lcd-1_47`, 4 MB flash override |

`sdkconfig.esp32-c6-lcd-1_47` is generated from `sdkconfig.defaults` — edit
the defaults, not the generated file. Do **not** enable `LV_COLOR_16_SWAP`:
the flush callback already byte-swaps and would double-swap.

## Architecture essentials (see CLAUDE.md for full detail)

Three FreeRTOS tasks from `app_main`: **lvgl** (prio 2), **button** (prio 3),
**pager** (prio 4), plus a fourth on the station path — **wifi_mon** (prio 2,
`wifi_manager_start_monitor()`) — none of which exist on the provisioning
path, where `app_main` diverts into `provision_start()` (AP + DNS hijack +
httpd) and never comes back. Network stack bottom-up: `net_conn.c` (+ optional
`socks5.c`) → `https_client.c` (raw mbedTLS, not `esp_http_client`) →
`telegram.c` (cJSON) → `pager_task.c`. `msg_queue.c` is the shared state
between button and pager and is mutex-guarded.

- **WiFi failover is the monitor's job once armed.** After the boot connect,
  `wifi_manager_start_monitor()` owns link recovery: 60 s background scans,
  proactive roam (threshold + margin + cooldown, anti-flap) and, on a drop,
  scan → rank configured networks by RSSI → reconnect to the best, retrying
  with backoff **forever** — never a reboot into the portal at runtime. The
  event handler's retry-then-FAIL logic is boot-time only; when armed it just
  flags the monitor. The pager poll needs no cooperation: it backs off 5 s
  until the link returns.

- **LVGL is not thread-safe.** Every `lv_*` from a non-lvgl task must be
  inside `display_lvgl_lock()`/`display_lvgl_unlock()`. The `ui_*` functions
  take the lock themselves — do **not** call them with it held.
- **`HTTPS_ERR_ABORTED` is normal**, not an error. The pager's abort hook
  tears down a long poll when an ack is queued; do not back off on it.
- **The display rotates in hardware** (ST7789 MADCTL), not in LVGL. That is
  why `BOARD_LCD_GAP_Y` is 34 and the gap is on Y, not X. Flip
  `BOARD_LCD_MIRROR_X/Y` in `app_config.h` if a unit shows up upside-down.
- **Backlight policy is in `screen.c`**: the glass lights only on a BOOT press
  (held while the queue is non-empty, then a `UI_SCREEN_ON_MS` grace) — arrivals
  do **not** light it. The RGB LED on GPIO8 (`led.c`) is the unread cue, and
  `display.c` only switches the GPIO.

## Load-bearing things that look incidental

These are commented at the site; do not "clean them up":

- HTTP/1.0 on purpose → excludes chunked encoding → read-until-close is an
  exact body read (no chunk parser).
- `APP_HTTP_SOCKET_TIMEOUT_MS` must stay above `APP_LONGPOLL_TIMEOUT_S` or a
  quiet poll looks like a dead connection.
- The update offset advances past dropped updates (commands, no-chat
  messages); stalling it makes Telegram replay the batch forever.
- **The message queue **drops the oldest when full** and counts drops (shown
  in the header). A dropped message can never be acked.
- **The message queue persists itself to NVS** (`msg_store.c`): every push
  writes its slot blob then meta, every pop writes just meta. `init()` loads
  first and wipes on a magic/version/size mismatch. Slot is written before
  meta so a power cut between them leaves the orphaned blob outside the live
  range, to be overwritten on the next push to that index.
- `pager_msg_t` is ~1 KB; neither queue slots nor the pager poll batch may
  live on a task stack (the batch is `static`, button stack sized for two).
  Raising `APP_MSG_QUEUE_LEN` past 64 risks heap exhaustion at TLS handshake.
- Body scroll restarts **only when the queue head changes**, keyed on
  `(chat_id, message_id, inline_message_id)`; `ui_render_queue()` repaints that
  leave the head alone must not jerk the text. `ui_init` paints the empty-queue
  state itself because of that short-circuit.
- **Inline mode has no chat id, and everything odd about it follows from that**
  (`inline_mode.c`, "Inline mode" in CLAUDE.md): a page sent inline is marked by
  editing its own message, not with a reaction; the `⏳` button exists only
  because Telegram withholds `inline_message_id` from a result with no keyboard;
  a `chosen_inline_result` is a page only when `result_id` is
  `TELEGRAM_INLINE_RESULT_PAGE`; the message id is a synthetic negative counter
  and the date is `time(NULL)`, because `ChosenInlineResult` supplies neither.
  Enabling it is a BotFather matter (`/setinline` **and** `/setinlinefeedback`),
  not a firmware one.
- **Receipts are reactions with a text fallback** (`mark_chat_message()`): `👌`
  delivered, `👀` read, one reaction per bot so the second replaces the first.
  `✅` is *not* in Telegram's fixed 73-emoji reaction set — do not "fix" the
  delivered emoji to a check mark. Channels and opted-out chats refuse
  reactions, hence the fallback to the old `STR_RECEIPT_*` reply.
- Text is UTF-8 truncated on character boundaries (`copy_utf8` in
  `telegram.c`); `APP_MSG_TEXT_MAX` is bytes (Cyrillic = 2/char). This, not
  the scroll, is what caps a long message — a tighter cut looks like a scroll
  that stops early.
- **SD logging (`sd_log.c`) is best-effort by contract**: no card, failed mount
  or failed write is logged and ignored — it must never block the pager, restart
  the device, or drop a page from the queue. The card shares SPI2 with the LCD,
  so the bus is set up once in `display_init()` (MISO is in the bus config for
  the card's sake, not the write-only panel's).
- **`display_spi_bus_lock()` is what actually keeps the card and the panel off
  each other** — the SPI driver's own bus lock is *not* enough, and this is the
  reboot loop that appears once enough pages are queued. A flush *queues* its
  pixel transfer and releases the bus while the DMA is still running, and the
  card walking in there sets up a transaction on a busy peripheral (silent
  assertions turn that into a bare `abort() was called at PC …`). So: the flush
  path takes the guard in `lvgl_flush_cb()` and gives it back **from the
  transfer-done ISR** — hence a binary semaphore, not a mutex — with a
  hand-back on a failed `draw_bitmap()` since no interrupt is coming for a
  transfer that was never queued; `sd_log.c` holds it across a whole card
  operation, mount to `fclose()`, not per call. **Nothing under the guard may
  take the LVGL lock** (the LVGL task waits for the guard while holding it, so
  the reverse order deadlocks). Backlight (LEDC) and RGB LED (RMT) stay
  outside it — neither touches SPI2.
- **The getUpdates offset is persisted too** (`msg_store_save_offset()` /
  `load_offset()`, key `offset` in the same `pager` namespace). Kept only in
  RAM it makes every reboot re-download the batch it died in and push a
  *second* copy of pages that were already queued and persisted, so the queue
  grows by a message per restart. `pager_task` writes it as the **last** thing
  in a lap — after the batch is on the card, in the queue and in flash — so a
  crash above that line replays the batch, which is the right way round; that
  is why the write does not live where `telegram_poll()` advances the value.
  `msg_store_clear()` does not touch it: a queue-layout change says nothing
  about what Telegram has already handed over.
- **`/status`'s chip temperature reads the die, not the room** (`commands.c`,
  `format_chip_temp()`, needs `esp_driver_tsens` in `REQUIRES`). The sensor is
  installed and uninstalled inside the one call — `/status` is minutes apart at
  best and a handle held open keeps the analogue front end powered — and any
  failure prints "unavailable" rather than dropping the line.
- **`STR_*` are runtime lookups, not literals** — never concatenate them with
  adjacent text or an `LV_SYMBOL_*`; format with `%s` (`ui_set_statusf()`,
  split `append()` calls in `commands.c`). See "UI strings" below.
- The setup screen reuses the pager's own widgets (`ui_show_provision()`), and
  cancels the body scroll animation so the URL cannot crawl out of view.

## Adding a chat command

`commands_try_handle()` in `commands.c` answers only the names it lists;
every other `/…` message is paged as ordinary text. Adding one means: a new
branch there, plus a `STR_CMD_*` line in the `UI_STRING_LIST` X-macro in
`ui_strings.h` (English *and* Russian columns) and a matching `#define STR_*`
accessor. Use `command_arg()` if the command takes an argument and
`is_command()` if it does not — both share one parser, so `/statuses` cannot
answer as `/status`. Keep the reply buffer `static` (the TLS handshake runs on
the pager stack while it is live), and keep `pager_task`'s skip of
`idle_status()` on a command-answering lap — otherwise the idle text wipes the
footer feedback before it can be read.

**`/pager <text>` is not like the others** and `msg` is non-const because of
it: it strips its own prefix in place and returns `COMMAND_NONE`, so the text
is paged as an ordinary message instead of answered. It exists so a group can
page the device without the bot's privacy mode being turned off — under privacy
mode a bot in a group only sees messages that address it, and a command always
does. Do not "tidy" it into a normal answering branch.

## Adding an inline card

`inline_answer_query()` in `inline_mode.c` builds the cards; `telegram.c` owns
the JSON. A card is one `telegram_inline_result_t`: `id`, `title`, optional
`description`, the `text` that gets posted, and `track` — set `track` only for a
card that pages the device, since it is what attaches the `⏳` button and buys
back an `inline_message_id`. A card whose `id` is not
`TELEGRAM_INLINE_RESULT_PAGE` is answered and then ignored when picked, which is
what keeps the status cards from paging the device with its own report. Strings
go in `ui_strings.h` like everything else; they are sent to Telegram rather than
drawn, so they may use emoji the pager fonts do not carry.

## UI strings

Every firmware-authored string lives in `src/ui_strings.h` in the single
`UI_STRING_LIST(X)` X-macro — one `X(NAME, english, russian)` line per string,
which generates the `str_id_t` enum and (in `ui_strings.c`) the two parallel
language tables, so they cannot drift apart. A new string means one new `X(...)`
line and one `#define STR_NAME ui_str(STR_ID_NAME)` accessor. The active table
is picked at runtime by `ui_set_language()` (called from `main.c` after
`settings_load()`); `STR_*` are **function calls through `ui_str()`, not
literals**, so they must never be string-literal-concatenated with adjacent
text or an `LV_SYMBOL_*` — format with `%s` instead (see `ui_set_statusf()`).
Receipt texts (`STR_RECEIPT_*`) are escaped byte sequences with the readable
form in a trailing `/* … */` comment — keep both in sync. `LV_SYMBOL_*` icons
are passed at call sites, never baked into the macro list (the header stays
LVGL-free).
