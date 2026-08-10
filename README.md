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
- **Long messages scroll themselves** — a page taller than the body area crawls
  down, pauses, rewinds and repeats, so it can be read with no touch screen.
- **English or Russian interface**, picked in `.env` — see below.
- **Cyrillic** message text throughout, read **horizontally**: the panel runs rotated to
  320×172 so Russian sentences wrap across the long edge instead of down a narrow column.
- **`/status` and `/ping` from the chat** — ask the device how it is doing
  without a serial cable; see below.
- Optional **SOCKS5 proxy**, with username/password authentication, for the
  Telegram connection.

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
in whichever `UI_LANGUAGE` the firmware was built with.

There is no allow-list here, for the same reason there is none on messages: the
bot token is the access control, and whoever can page the device can also ask
how it is doing. The proxy endpoint is deliberately *not* in the reply — only
whether one is in use.

## Hardware

Nothing to wire: the display and the button are both on the board.

| Function | GPIO | Notes |
| --- | --- | --- |
| LCD MOSI / SCLK / CS / DC / RST / BL | 6 / 7 / 14 / 15 / 21 / 22 | ST7789, SPI2 |
| "Read" button | 9 | the BOOT key, active low |

Pins live in `src/app_config.h`. If the image comes out upside down, flip
`BOARD_LCD_MIRROR_X` / `BOARD_LCD_MIRROR_Y` there.

## Setup

1. Create a bot with [@BotFather](https://t.me/BotFather) and copy the token.
2. `cp example.env .env` and fill in the token and WiFi credentials. There is no
   chat id: anyone who writes to the bot is paged, so the token is what keeps the
   pager yours — keep it private, and consider turning the bot's group access off
   in @BotFather so it cannot be added to strangers' groups.
3. `pio run -t upload -e esp32-c6-lcd-1_47`
4. `pio device monitor`

`src/secrets.h` is generated from `.env` before every build. Both are gitignored.

### Language

```ini
UI_LANGUAGE=english   # or: russian
```

This picks the language of everything the device itself writes — the status
line, the "no new messages" placeholder, and the "read"/"delivered" receipts it
sends back to the chat. Message text and sender names always arrive from
Telegram as-is, so a device built with `english` still displays Russian
messages normally.

The choice is made at compile time, so changing it means a rebuild and reflash.
The strings themselves are in `src/ui_strings.h`, one block per language.

### Proxy

```ini
PROXY_TYPE=socks5
PROXY_HOST=1.2.3.4
PROXY_PORT=1080
PROXY_USER=user     # omit both for an open proxy
PROXY_PASS=secret
```

The hostname is resolved by the proxy rather than locally, which is usually the
point of having one.

> **MTProto proxies cannot work here.** An MTProto proxy relays Telegram's
> *client* protocol to Telegram's data centres. The Bot API is ordinary HTTPS to
> `api.telegram.org`, which an MTProto proxy has no way to carry. Supporting one
> would mean writing a full MTProto client — DH key exchange, AES-IGE, TL
> serialisation, DC migration — rather than making an HTTPS request. Use SOCKS5.
