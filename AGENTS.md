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
- **No runtime language switch, no allow-list, no pinned TLS cert.** See
  `CLAUDE.md` for the reasoning; do not add any of them.

## Secrets and codegen (build will fail without this)

- A build requires `.env` (copy `example.env`). `tools/gen_secrets.py` runs as
  a PlatformIO **pre**-build hook and regenerates `src/secrets.h` every time.
  Both files are gitignored. Never hand-edit `src/secrets.h` or commit either.
  The script hard-fails on: missing `BOT_TOKEN`/`WIFI_SSID`/`WIFI_PASSWORD`,
  unknown `UI_LANGUAGE`, half-filled proxy config, or `PROXY_TYPE=mtproto`.
- `src/pager_font_14.c` / `pager_font_20.c` are generated **and committed**.
  Normal builds do **not** regenerate them. Only `bash tools/gen_fonts.sh`
  does — it needs Node (npx) **and** `managed_components/lvgl__lvgl/` present,
  so run a build once before regenerating. The `--no-compress` flag is
  mandatory: `CONFIG_LV_USE_FONT_COMPRESSED` is off, so compressed glyphs
  decode to garbage.

## Editor / clangd

`.clangd` points at `.pio/build/esp32-c6-lcd-1_47/compile_commands.json`.
Run `pio run` once before expecting completion on ESP-IDF/LVGL headers;
after `fullclean` completion is broken until the next build regenerates it.

## Config surfaces (where a change actually goes)

| File | Holds |
| --- | --- |
| `src/app_config.h` | board pins, queue sizes, timeouts, Telegram tuning |
| `src/ui_strings.h` | per-language string tables (see below) |
| `sdkconfig.defaults` | console USB-Serial-JTAG, RGB565 + manual byte swap, full mbedTLS bundle, 16 kB TLS in-buffer, 4 MB flash |
| `partitions.csv` | custom table (LVGL+mbedTLS+WiFi > the default 1 MB app slot); 64 KB NVS for the persisted queue |
| `platformio.ini` | one env `esp32-c6-lcd-1_47`, 4 MB flash override, pre-build script |

`sdkconfig.esp32-c6-lcd-1_47` is generated from `sdkconfig.defaults` — edit
the defaults, not the generated file. Do **not** enable `LV_COLOR_16_SWAP`:
the flush callback already byte-swaps and would double-swap.

## Architecture essentials (see CLAUDE.md for full detail)

Three FreeRTOS tasks from `app_main`: **lvgl** (prio 2), **button** (prio 3),
**pager** (prio 4). Network stack bottom-up: `net_conn.c` (+ optional
`socks5.c`) → `https_client.c` (raw mbedTLS, not `esp_http_client`) →
`telegram.c` (cJSON) → `pager_task.c`. `msg_queue.c` is the shared state
between button and pager and is mutex-guarded.

- **LVGL is not thread-safe.** Every `lv_*` from a non-lvgl task must be
  inside `display_lvgl_lock()`/`display_lvgl_unlock()`. The `ui_*` functions
  take the lock themselves — do **not** call them with it held.
- **`HTTPS_ERR_ABORTED` is normal**, not an error. The pager's abort hook
  tears down a long poll when an ack is queued; do not back off on it.
- **The display rotates in hardware** (ST7789 MADCTL), not in LVGL. That is
  why `BOARD_LCD_GAP_Y` is 34 and the gap is on Y, not X. Flip
  `BOARD_LCD_MIRROR_X/Y` in `app_config.h` if a unit shows up upside-down.
- **Backlight policy is in `screen.c`** (lit ⇔ queue non-empty, plus a
  `UI_SCREEN_ON_MS` grace). `display.c` only switches the GPIO.

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
  `(chat_id, message_id)`; `ui_render_queue()` repaints that leave the head
  alone must not jerk the text. `ui_init` paints the empty-queue state itself
  because of that short-circuit.
- Text is UTF-8 truncated on character boundaries (`copy_utf8` in
  `telegram.c`); `APP_MSG_TEXT_MAX` is bytes (Cyrillic = 2/char). This, not
  the scroll, is what caps a long message — a tighter cut looks like a scroll
  that stops early.

## Adding a chat command

`commands_try_handle()` in `commands.c` answers only the names it lists;
every other `/…` message is paged as ordinary text. Adding one means: a new
branch there, plus the `STR_CMD_*` strings in **both** language blocks of
`ui_strings.h`. Keep the reply buffer `static` (the TLS handshake runs on
the pager stack while it is live), and keep `pager_task`'s skip of
`idle_status()` on a command-answering lap — otherwise the idle text wipes
the footer feedback before it can be read.

## UI strings

Every firmware-authored string lives in `src/ui_strings.h` as one `STR_*`
macro per language block; nothing user-facing belongs in a `.c`. `UI_LANGUAGE`
in `.env` picks the block at compile time via `SECRET_UI_LANGUAGE_ID` —
exactly one table is ever built. A new string goes in **both** blocks; a
missing one is a compile error (intentional). Receipt texts
(`STR_RECEIPT_*`) are escaped byte sequences with the readable form in a
trailing comment — keep both in sync. `LV_SYMBOL_*` icons are concatenated
at call sites, never baked into macros (the header must stay LVGL-free).
