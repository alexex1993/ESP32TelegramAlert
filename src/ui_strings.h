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

#else
#error "Unknown SECRET_UI_LANGUAGE_ID -- UI_LANGUAGE in .env must be english or russian"
#endif
