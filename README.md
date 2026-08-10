# ESP32TelegramAlert

<img width="1362" height="788" alt="image" src="https://github.com/user-attachments/assets/5c701e10-aea1-4739-8836-ceeec23b74d7" />

[YOUTUBE DEMO](https://www.youtube.com/shorts/OFUGBKv261c)

A Telegram pager for the **Waveshare ESP32-C6-LCD-1.47**: a bot forwards
messages to the device, they queue up on the screen in landscape, and the
board's BOOT key sends a "read" receipt back to the chat.

This is a ground-up ESP-IDF rewrite of the Arduino sketch at
[alexex1993/TelegramPagerESP32](https://github.com/alexex1993/TelegramPagerESP32),
which drove an SSD1306 OLED via the FastBot library.

## Features

- Long-polls the Telegram Bot API and pages every message anyone sends the bot —
  no chat id to configure, receipts go back to whoever wrote.
- **Message queue** — up to 32 unread messages wait their turn; the header shows how many.
- **BOOT key sends "✅ Read"** as a reply to the message on screen, then advances to the next one.
- Replies "📨 Delivered" the moment a message arrives (as the original pager did).
- **The screen sleeps on its own** — the backlight is lit only while something is
  unread, plus a 20 s grace window; a BOOT press wakes it without acknowledging
  anything.
- **The RGB LED is the "you have mail" cue** — the on-board pixel blinks while
  anything is unread, so an arriving message never has to light the glass.
- **Unread messages survive a power cut** — the queue is written to flash (NVS)
  on every push and pop, so unplugging the pager does not lose a page or the
  right to acknowledge it.
- **Every page is archived to the TF card**, one file per message under
  `/TelegramPager/<chat id>/<date>/<time>.txt`. Optional: with no card in the
  slot the pager runs exactly as before.
- **Long messages scroll themselves** — a page taller than the body area crawls
  down, pauses, rewinds and repeats, so it can be read with no touch screen.
- **English or Russian interface**, picked at runtime through the setup portal — see below.
- **Cyrillic** message text throughout, read **horizontally**: the panel runs rotated to
  320×172 so Russian sentences wrap across the long edge instead of down a narrow column.
- **`/status` and `/ping` from the chat** — ask the device how it is doing
  without a serial cable; see below.
- Optional **SOCKS5 proxy**, with username/password authentication, for the
  Telegram connection.
- **Captive-portal WiFi provisioning** — flash one image to every device, then
  configure each over WiFi on first boot. No `.env`, no per-device build.

## Chat commands

Two messages are answered by the firmware instead of being paged: they never
reach the screen, take a queue slot or wait for a button press.

| Command | Answer |
| --- | --- |
| `/ping` | `🏓 pong` — the poll loop is alive and the bot token still works. |
| `/status` | Firmware revision and build date, uptime, SSID / RSSI / channel, IP, whether a proxy is in use, queue depth (and messages dropped by overflow), backlight state, free heap, the reason for the last reset, and the device clock. |

```
📟 Pager status
Firmware: addb85e (ESP-IDF 6.0.1)
Built: Aug 10 2026 18:20:21
Uptime: 2d 03:14:07
WiFi: my-network, -58 dBm, ch 6
IP: 192.168.1.42
Proxy: SOCKS5
Queue: 3/8
Screen: on
Heap: 84 KB free, 61 KB min
Last reset: POWERON
Clock: 2026-08-10 21:20 UTC+3
```

Both work as `/status@yourbot` too, which is the form Telegram sends in groups.
Anything else starting with `/` is an ordinary message and gets paged — someone
typing `/dev/ttyUSB0` is sending a page, not driving the pager. The answers are
in whichever language the device was configured for at setup time.

There is no allow-list here, for the same reason there is none on messages: the
bot token is the access control, and whoever can page the device can also ask
how it is doing. The proxy endpoint is deliberately *not* in the reply — only
whether one is in use.

## Hardware

Nothing to wire: the display and the button are both on the board.

| Function | GPIO | Notes |
| --- | --- | --- |
| LCD MOSI / SCLK / CS / DC / RST / BL | 6 / 7 / 14 / 15 / 21 / 22 | ST7789, SPI2 |
| TF card CS / MISO | 4 / 5 | shares SPI2 with the LCD; optional |
| RGB LED | 8 | one WS2812, the unread indicator |
| "Read" button | 9 | the BOOT key, active low |

Pins live in `src/app_config.h`. If the image comes out upside down, flip
`BOARD_LCD_MIRROR_X` / `BOARD_LCD_MIRROR_Y` there.

## Setup

Settings live in the device's NVS, not in a build-time file, so **one firmware
image flashes to every device** and each is configured over WiFi.

1. Create a bot with [@BotFather](https://t.me/BotFather) and copy the token.
2. Build and flash:
   ```
   pio run -t upload -e esp32-c6-lcd-1_47
   ```
3. On first boot the pager opens an open WiFi network named **`TelegramPager`**.
   The screen shows its address — `http://192.168.4.1` — and the two steps to
   follow. Join that WiFi from a phone and the captive-portal sheet pops up
   automatically (or open `http://192.168.4.1` by hand).
4. Fill in the form and press **Save & reboot**. The pager stores the settings,
   restarts, joins your WiFi and starts paging.

| Field | Required | Notes |
| --- | --- | --- |
| Bot token | yes | as @BotFather gives it, `123456:AA…` |
| WiFi name (SSID) | yes | 2.4 GHz — the ESP32-C6 has no 5 GHz radio |
| WiFi password | no | leave empty for an open network |
| Timezone | yes | UTC offset in hours, −12…14; drives the `[HH:MM]` stamp, the card's folder names and `/status` |
| Language | yes | English or Русский, for what the device itself writes |
| Proxy | no | `None`, or `SOCKS5` + host, port and optional user/password |

The form is validated before anything is stored, so a half-filled proxy or a
malformed token comes back with a message instead of bricking the next boot.

**If the captive-portal sheet does not pop up** (some Android builds, or a phone
that keeps mobile data on), just open `http://192.168.4.1` in a browser while
joined to `TelegramPager` — the screen shows that address for exactly this case.
The setup network is deliberately open: the secret is what you type *into* the
form, and a WiFi password here would have to be displayed on the pager's screen
before you could reach the page anyway.

There is no chat id to configure: anyone who writes to the bot is paged, so the
token is what keeps the pager yours — keep it private, and consider turning the
bot's group access off in @BotFather so it cannot be added to strangers' groups.

### Re-provisioning

To change the WiFi (or any setting) later: with the pager running, **hold the
BOOT key for ~5 seconds**. It reboots back into the `TelegramPager` AP, the form
pre-filled with the current values — token and password included, so you only
edit what changed. If the entered WiFi credentials ever fail to connect after
all retries, the pager falls back to the same AP on its own rather than
boot-looping on a dead link.

(The hold starts as an ordinary press, so if a message was waiting on screen it
gets acknowledged on the way — the receipt is honest, you did read it.)

Unread messages are kept in flash, so they are still queued after the reboot.

The settings module is `src/settings.c` (NVS namespace `cfg`); the portal is
`src/provision.c`.

### Language

Language is chosen on the setup form (English / Русский) and stored at runtime,
so the same image can be either. It affects only what the device itself writes —
the status line, the "no new messages" placeholder, and the "read"/"delivered"
receipts it sends back to the chat. Message text and sender names always arrive
from Telegram as-is, so an English-configured pager still displays Russian
messages normally.

The strings are in `src/ui_strings.h` as an X-macro list (`UI_STRING_LIST`) that
generates both language tables and the id enum in lockstep, and resolved at
runtime by `ui_str()` in `src/ui_strings.c`.

### Message log on the TF card

Insert a FAT-formatted microSD and every paged message is also written to it,
one file per message:

```
/TelegramPager/<chat id>/2026-08-10/21-20-07.txt
```

Each file holds the sender, the message's own Telegram timestamp (shifted by
the timezone you configured, so it matches the `[HH:MM]` on screen), the chat
and message ids, and the full text. Logging is best-effort: no card, a
write-protected card or a failed write is reported to the serial log and
otherwise ignored — it never delays a page or a receipt. The card shares the
LCD's SPI bus, so nothing extra is wired.

### Proxy

```ini
Proxy: SOCKS5
Proxy host: 1.2.3.4
Proxy port: 1080
Proxy user: user     # omit both for an open proxy
Proxy password: secret
```

Set on the same setup form. The hostname is resolved by the proxy rather than
locally, which is usually the point of having one.

> **MTProto proxies cannot work here.** An MTProto proxy relays Telegram's
> *client* protocol to Telegram's data centres. The Bot API is ordinary HTTPS to
> `api.telegram.org`, which an MTProto proxy has no way to carry. Supporting one
> would mean writing a full MTProto client — DH key exchange, AES-IGE, TL
> serialisation, DC migration — rather than making an HTTPS request. Use SOCKS5.
