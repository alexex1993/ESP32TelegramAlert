#pragma once

#include "secrets.h"

// Every user-facing string in one place, selected at compile time by
// UI_LANGUAGE in .env. There is no runtime switch: the language is a build
// setting, so only one table is ever compiled in.
//
// These are the strings the *device* produces. Message text and sender names
// come from Telegram and are passed through untouched, which is why the fonts
// keep the Cyrillic block whatever this is set to -- see tools/gen_fonts.sh.
//
// LV_SYMBOL_* icons are appended at the call sites rather than baked in here,
// so this header stays free of an LVGL dependency (telegram.c includes it).

#define APP_LANG_EN 0
#define APP_LANG_RU 1

#ifndef SECRET_UI_LANGUAGE_ID
#error "SECRET_UI_LANGUAGE_ID missing from secrets.h -- rebuild so tools/gen_secrets.py regenerates it from .env"
#endif

#if SECRET_UI_LANGUAGE_ID == APP_LANG_RU

// ---- Boot ---------------------------------------------------------------
#define STR_WIFI_CONNECTING     "Подключение к WiFi..."
#define STR_WIFI_FAILED         "WiFi недоступен, перезагрузка..."
#define STR_TIME_SYNCING        "Синхронизация времени..."

// ---- Message area -------------------------------------------------------
#define STR_NO_MESSAGES         "Нет новых сообщений"
// Shown when a message carries no text at all (a sticker, a location, ...).
#define STR_NO_TEXT_PLACEHOLDER "[вложение без текста]"

// ---- Status line --------------------------------------------------------
#define STR_WAITING             "Ожидание сообщений"
#define STR_WAITING_PROXY       "Ожидание сообщений (SOCKS5)"
#define STR_NEW_MESSAGES_FMT    "Новых: %d"
#define STR_NOTHING_TO_ACK      "Нечего подтверждать"
#define STR_ACK_SENDING         "Отправляю «прочитано»..."
#define STR_ACK_SENT            "Подтверждено"
#define STR_ACK_FAILED          "Подтверждение не доставлено"
#define STR_ACK_QUEUE_FULL      "Очередь подтверждений переполнена"
#define STR_TELEGRAM_OFFLINE    "Нет связи с Telegram, повтор..."

// ---- Receipts sent back to the chat -------------------------------------
// Spelled as escaped bytes with the readable form alongside; keep both in sync.
#define STR_RECEIPT_READ      "\xE2\x9C\x85 \xD0\x9F\xD1\x80\xD0\xBE\xD1\x87\xD0\xB8\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE" // "✅ Прочитано"
#define STR_RECEIPT_DELIVERED "\xF0\x9F\x93\xA8 \xD0\x94\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD0\xB2\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xBE" // "📨 Доставлено"

// ---- Answers to chat commands -------------------------------------------
// These go to Telegram, not to the glass, so they may use characters the
// pager fonts do not carry -- the leading emoji are spelled as escaped bytes
// following the receipt convention above.
#define STR_CMD_PONG            "\xF0\x9F\x8F\x93 pong" // "🏓 pong"
#define STR_CMD_STATUS_TITLE    "\xF0\x9F\x93\x9F \xD0\xA1\xD0\xBE\xD1\x81\xD1\x82\xD0\xBE\xD1\x8F\xD0\xBD\xD0\xB8\xD0\xB5 \xD0\xBF\xD0\xB5\xD0\xB9\xD0\xB4\xD0\xB6\xD0\xB5\xD1\x80\xD0\xB0" // "📟 Состояние пейджера"
#define STR_CMD_STATUS_FIRMWARE "Прошивка"
#define STR_CMD_STATUS_BUILT    "Собрана"
#define STR_CMD_STATUS_UPTIME   "Аптайм"
#define STR_CMD_STATUS_WIFI     "WiFi"
// SSID, RSSI in dBm, channel.
#define STR_CMD_STATUS_WIFI_FMT "%s, %d дБм, канал %u"
#define STR_CMD_STATUS_IP       "IP"
#define STR_CMD_STATUS_DISCONNECTED "нет соединения"
#define STR_CMD_STATUS_PROXY    "Прокси"
#define STR_CMD_STATUS_PROXY_NONE "напрямую"
#define STR_CMD_STATUS_QUEUE    "Очередь"
#define STR_CMD_STATUS_DROPPED  "потеряно"
#define STR_CMD_STATUS_SCREEN   "Экран"
#define STR_CMD_STATUS_SCREEN_ON  "горит"
#define STR_CMD_STATUS_SCREEN_OFF "погашен"
#define STR_CMD_STATUS_HEAP     "Память"
// Free now, and the low-water mark since boot.
#define STR_CMD_STATUS_HEAP_FMT "%u КБ свободно, минимум %u КБ"
#define STR_CMD_STATUS_RESET    "Последний сброс"
#define STR_CMD_STATUS_CLOCK    "Часы"
#define STR_CMD_STATUS_CLOCK_UNSYNCED "не синхронизированы"

// ---- Status line, after a command ---------------------------------------
#define STR_CMD_ANSWERED        "Команда выполнена"
#define STR_CMD_FAILED          "Ответ на команду не доставлен"

#elif SECRET_UI_LANGUAGE_ID == APP_LANG_EN

// ---- Boot ---------------------------------------------------------------
#define STR_WIFI_CONNECTING     "Connecting to WiFi..."
#define STR_WIFI_FAILED         "WiFi unavailable, restarting..."
#define STR_TIME_SYNCING        "Syncing time..."

// ---- Message area -------------------------------------------------------
#define STR_NO_MESSAGES         "No new messages"
// Shown when a message carries no text at all (a sticker, a location, ...).
#define STR_NO_TEXT_PLACEHOLDER "[attachment without text]"

// ---- Status line --------------------------------------------------------
#define STR_WAITING             "Waiting for messages"
#define STR_WAITING_PROXY       "Waiting for messages (SOCKS5)"
#define STR_NEW_MESSAGES_FMT    "New: %d"
#define STR_NOTHING_TO_ACK      "Nothing to acknowledge"
#define STR_ACK_SENDING         "Sending \"read\"..."
#define STR_ACK_SENT            "Acknowledged"
#define STR_ACK_FAILED          "Receipt not delivered"
#define STR_ACK_QUEUE_FULL      "Receipt queue full"
#define STR_TELEGRAM_OFFLINE    "No Telegram connection, retrying..."

// ---- Receipts sent back to the chat -------------------------------------
// Spelled as escaped bytes with the readable form alongside; keep both in sync.
#define STR_RECEIPT_READ      "\xE2\x9C\x85 Read"        // "✅ Read"
#define STR_RECEIPT_DELIVERED "\xF0\x9F\x93\xA8 Delivered" // "📨 Delivered"

// ---- Answers to chat commands -------------------------------------------
// These go to Telegram, not to the glass, so they may use characters the
// pager fonts do not carry -- the leading emoji are spelled as escaped bytes
// following the receipt convention above.
#define STR_CMD_PONG            "\xF0\x9F\x8F\x93 pong" // "🏓 pong"
#define STR_CMD_STATUS_TITLE    "\xF0\x9F\x93\x9F Pager status" // "📟 Pager status"
#define STR_CMD_STATUS_FIRMWARE "Firmware"
#define STR_CMD_STATUS_BUILT    "Built"
#define STR_CMD_STATUS_UPTIME   "Uptime"
#define STR_CMD_STATUS_WIFI     "WiFi"
// SSID, RSSI in dBm, channel.
#define STR_CMD_STATUS_WIFI_FMT "%s, %d dBm, ch %u"
#define STR_CMD_STATUS_IP       "IP"
#define STR_CMD_STATUS_DISCONNECTED "disconnected"
#define STR_CMD_STATUS_PROXY    "Proxy"
#define STR_CMD_STATUS_PROXY_NONE "direct"
#define STR_CMD_STATUS_QUEUE    "Queue"
#define STR_CMD_STATUS_DROPPED  "dropped"
#define STR_CMD_STATUS_SCREEN   "Screen"
#define STR_CMD_STATUS_SCREEN_ON  "on"
#define STR_CMD_STATUS_SCREEN_OFF "off"
#define STR_CMD_STATUS_HEAP     "Heap"
// Free now, and the low-water mark since boot.
#define STR_CMD_STATUS_HEAP_FMT "%u KB free, %u KB min"
#define STR_CMD_STATUS_RESET    "Last reset"
#define STR_CMD_STATUS_CLOCK    "Clock"
#define STR_CMD_STATUS_CLOCK_UNSYNCED "not synced"

// ---- Status line, after a command ---------------------------------------
#define STR_CMD_ANSWERED        "Command answered"
#define STR_CMD_FAILED          "Command reply not delivered"

#else
#error "Unknown SECRET_UI_LANGUAGE_ID -- UI_LANGUAGE in .env must be english or russian"
#endif
