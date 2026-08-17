# ESP32TelegramAlert

A Telegram pager for the **Waveshare ESP32-C6-LCD-1.47**: a bot forwards
messages to the device, they queue up on the screen in landscape, and the
board's BOOT key sends a "read" receipt back to the chat.

This is a ground-up ESP-IDF rewrite of the Arduino sketch at
[alexex1993/TelegramPagerESP32](https://github.com/alexex1993/TelegramPagerESP32),
which drove an SSD1306 OLED via the FastBot library.

<img width="711" height="410" alt="image" src="https://github.com/user-attachments/assets/97c305d6-6cfa-4c77-a519-999c6795060e" />

## Demonstration
[YOUTUBE DEMO #1](https://www.youtube.com/shorts/OFUGBKv261c)

[YOUTUBE DEMO #2](https://www.youtube.com/shorts/CHrGNBdmHEU)

## Online Flasher

You can flash the latest firmware on this [website](https://pager.alexnew.ru/).

## Features

- Long-polls the Telegram Bot API and pages every message anyone sends the bot —
  no chat id to configure, receipts go back to whoever wrote.
- **Message queue** — up to 32 unread messages wait their turn; the header shows how many.
- **The BOOT key marks the message on screen 👀 read**, then advances to the next one.
- **Marks it 👌 delivered the moment it arrives** (as the original pager did). Both
  marks are *reactions* on the message rather than replies, so a busy chat does
  not fill up with receipts — with the old text replies kept as a fallback
  wherever reactions are refused.
- **Inline mode** — type `@yourbot` in *any* chat, with the bot not in it, to page
  the device or to drop its status into the conversation; see below.
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
- **`/pager <text>`** — pages the device with `<text>`, which is how to reach it
  from a group without turning the bot's privacy mode off.
- **`/last N`** — reads the newest unread pages back into the chat, so what is
  waiting on the pager can be checked from the phone. It only reads: nothing is
  acknowledged and nothing leaves the queue.
- **It says hello when it comes back** — after a power cut the pager tells
  everyone who has written to it that it is online again, so a silent pager is
  never mistaken for an idle one.
- Optional **SOCKS5 proxy**, with username/password authentication, for the
  Telegram connection.
- **Captive-portal WiFi provisioning** — flash one image to every device, then
  configure each over WiFi on first boot. No `.env`, no per-device build.


## Setup

Settings live in the device's NVS, not in a build-time file, so **one firmware
image flashes to every device** and each is configured over WiFi.

1. Create a bot with [@BotFather](https://t.me/BotFather) and copy the token.
2. Build and flash:
   ```
   pio run -t upload -e esp32-c6-lcd-1_47
   ```
   The PlatformIO platform is pinned in `platformio.ini` (`espressif32 @ 7.0.1`,
   ESP-IDF v6.0.1), so the build reproduces against a known toolchain.
   Or flash from a [browser](https://pager.alexnew.ru/)
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
| WiFi networks 2 and 3 | no | fallbacks: the pager monitors the list in the background and switches to the best available network automatically — at boot and at runtime |
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


## Chat commands

Some messages are handled by the firmware instead of being paged as they are:
they never reach the screen, take a queue slot or wait for a button press.

| Command | Answer |
| --- | --- |
| `/ping` | `🏓 pong` — the poll loop is alive and the bot token still works. |
| `/status` | Firmware revision and build date, uptime, SSID / RSSI / channel, IP, whether a proxy is in use, queue depth (and messages dropped by overflow), how many chats are on the announcement list, backlight state, free heap, the reason for the last reset, and the device clock. |
| `/last N` | The N newest unread pages, read back into the chat. See below. |
| `/pager <text>` | Not an answer at all — `<text>` is paged. See below. |

```
📟 Pager status
Firmware: addb85e (ESP-IDF 6.0.1)
Built: Aug 10 2026 18:20:21
Uptime: 2d 03:14:07
WiFi: my-network, -58 dBm, ch 6
IP: 192.168.1.42
Proxy: SOCKS5
Queue: 3/32
Contacts: 4/16
Screen: on
Heap: 84 KB free, 61 KB min
Last reset: POWERON
Clock: 2026-08-10 21:20 UTC+3
```

They all work as `/status@yourbot` too, which is the form Telegram sends in groups.
Anything else starting with `/` is an ordinary message and gets paged — someone
typing `/dev/ttyUSB0` is sending a page, not driving the pager. The answers are
in whichever language the device was configured for at setup time.

There is no allow-list here, for the same reason there is none on messages: the
bot token is the access control, and whoever can page the device can also ask
how it is doing. The proxy endpoint is deliberately *not* in the reply — only
whether one is in use.

### `/last N` — reading the queue from the chat

```
/last 3
```

```
📜 Last 3 of 7 unread

[08-16 09:12] Anna:
bins go out tonight

[08-16 10:40] Kitchen crew:
lunch at one?

[08-16 11:05] Anna:
got the keys, no rush
```

The N newest unread pages, oldest of them first, so the listing reads downwards
the way the chat around it does and the newest page ends up nearest your reply.
A bare `/last` gives three; the cap is **8** per reply, and asking for more is
clamped rather than refused — the title says how many of how many came back.
Anything that is not a plain number (`/last all`) gets the form back instead of a
guess.

**It reads and nothing more.** No page is popped, no receipt goes out and the
screen is not touched — the pages stay unread and still have to be cleared with
the BOOT key. Only that key means "I have seen this", and a listing must not
clear a page off the glass of a pager sitting in someone else's pocket. Long
pages are cut at ~320 bytes with a `…` so the whole listing fits in the single
message Telegram allows.

### `/pager <text>` — paging from a group

```
/pager buy bread on the way home
```

shows **buy bread on the way home** on the screen — the prefix is stripped, and
what is left is an ordinary page: your name above it, the time next to it, a
queue slot, the 👌/👀 marks and a BOOT press to clear it.

The point of it is groups. A bot with **privacy mode** on — @BotFather's default
— is only shown messages that address it, so a plain line of text in a group
never reaches the bot at all, and the pager stays silent. A command always
reaches it. So `/pager …` pages the device from a group without turning privacy
mode off, which you would otherwise have to do to make the bot see every message
in the room.

Writing to the bot in a private chat needs none of this — just send the text.
`/pager` on its own, with nothing after it, answers with a reminder of the form
rather than paging an empty page. `/pager@yourbot text` works too, which is how
Telegram rewrites commands in groups.

### Boot announcement

The pager remembers every chat that has written to it — pages and commands
alike — and once the WiFi is up after a restart it sends each of them a single
line:

```
📟 Pager is online
```

A pager that lost power otherwise leaves the people paging it unable to tell
"nobody has written" from "it has been off since Tuesday". The list holds 16
chats, keyed on the chat rather than the person (a group is one entry and is
told in the group), survives reboots in flash, and drops its least recently seen
entry when full. `/status` reports how many are on it. Pages sent through inline
mode belong to no chat, so they add nobody. Set `APP_ANNOUNCE_ON_BOOT` to `0` in
`src/app_config.h` to keep the list but stay quiet.

## Inline mode

Type `@yourbot` into the message box of **any** chat — a group you are in, a
private conversation, a channel — and the bot answers with a list you pick from,
without ever being a member of that chat.

| What you type | What you get |
| --- | --- |
| `@yourbot` | **📟 Pager status** and **🏓 Ping** — the same reports `/status` and `/ping` produce. Picking one posts it into the chat; the pager itself is not disturbed. |
| `@yourbot buy bread` | **📟 Send to pager** — picking it posts *buy bread* into the chat **and** pages the device with it. |

A page sent this way is marked in the chat as it travels, exactly like one sent
to the bot directly:

```
buy bread              ← picked from the list
[ ⏳ ]
buy bread 👌           ← the pager received it
buy bread 👀           ← the BOOT key was pressed
```

### Enabling it

Inline mode cannot be switched on from the firmware — there is no Bot API method
for it. Both of these are @BotFather settings, and **both** are needed:

1. `/setinline` → pick the bot → type a placeholder, e.g. `message for the pager`.
2. `/setinlinefeedback` → pick the bot → **Enabled**.

Step 2 is the one that is easy to miss: without it Telegram never tells the bot
which result was picked, so the message is posted into the chat and the pager
never hears about it.

### Things worth knowing

- **The ⏳ button is not decoration.** Telegram only reports back an identifier
  for a posted inline message if that message carries an inline keyboard, and
  without that identifier the page could never be marked delivered or read — the
  bot is never told which chat it landed in, so there is nothing to react *to*.
  Both marks edit the message and remove the button with it, so it is on screen
  only while the page is genuinely in flight. Tapping it meanwhile answers with
  "the pager has not received this yet".
- **Results take a moment to appear.** Every keystroke is a fresh query and this
  device answers each one over its own TLS handshake, so the list lags a second
  or so behind your typing. Only the newest query is answered; the ones you
  typed past are dropped rather than paid for.
- **`✅` is not available.** Telegram allows reactions only from a fixed list of
  73 emoji and a green check mark is not among them, which is why "delivered" is
  👌.
- **Inline mode widens who can reach the pager.** There is no allow-list here
  either — the token is the access control — but an inline query needs no chat
  with the bot at all, so anyone who shares a group with someone who has the bot
  can page the device. Leave inline mode off in @BotFather if that is not what
  you want; the firmware simply never sees those updates.

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

Language is chosen on the setup form (English/Russian) and stored at runtime,
so the same image can be either. It affects only what the device itself writes —
the status line, the "no new messages" placeholder, and the "read"/"delivered"
receipts it sends back to the chat. Message text and sender names always arrive
from Telegram as-is, so an English-configured pager still displays Russian
messages normally.

The strings are in `src/ui_strings.h` as an X-macro list (`UI_STRING_LIST`) that
generates both language tables and the id enum in lockstep, and resolved at
runtime by `ui_str()` in `src/ui_strings.c`.

### Message log on the Micro SD card

Insert a FAT-formatted microSD and every paged message is also written to it,
one file per message:

```
/TelegramPager/<chat id>/2026-08-10/21-20-07.txt
/TelegramPager/inline/2026-08-10/21-22-13.txt
```

Each file holds the sender, the message's own Telegram timestamp (shifted by
the timezone you configured, so it matches the `[HH:MM]` on screen), the chat
and message ids, and the full text. Pages sent through inline mode belong to no
chat, so they are filed under `inline/` and stamped with the device clock —
Telegram reports a picked inline result as an event, without a send time. Logging is best-effort: no card, a
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
