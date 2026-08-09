# ESP32TelegramAlert

A Telegram pager for the **Waveshare ESP32-C6-LCD-1.47**: a bot forwards
messages to the device, they queue up on the screen in landscape, and the
board's BOOT key sends a "read" receipt back to the chat.

This is a ground-up ESP-IDF rewrite of the Arduino sketch at
[alexex1993/TelegramPagerESP32](https://github.com/alexex1993/TelegramPagerESP32),
which drove an SSD1306 OLED via the FastBot library.

## Features

- Long-polls the Telegram Bot API and pages every message from one allow-listed chat.
- **Message queue** — up to 8 unread messages wait their turn; the header shows how many.
- **BOOT key sends "✅ Read"** as a reply to the message on screen, then advances to the next one.
- Replies "📨 Delivered" the moment a message arrives (as the original pager did).
- **English or Russian interface**, picked in `.env` — see below.
- **Cyrillic** message text throughout, read **horizontally**: the panel runs rotated to
  320×172 so Russian sentences wrap across the long edge instead of down a narrow column.
- Optional **SOCKS5 proxy**, with username/password authentication, for the
  Telegram connection.

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
2. `cp example.env .env` and fill in the token, your chat id, and WiFi credentials.
   Messages from any other chat are ignored — send the bot a message and read the
   rejected chat id out of the serial log if you do not know yours.
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

 (needs Node.js) after changing sizes or ranges.
