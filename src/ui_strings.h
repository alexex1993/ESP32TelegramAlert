#pragma once

#include <stdbool.h>

// Every user-facing string, in both English and Russian, selected at RUNTIME
// (not compile time) so a single image can be configured for either language
// through the provisioning portal. The language is an app setting (see
// settings.h) applied once at boot via ui_set_language(); message text and
// sender names always come from Telegram untouched.
//
// The list below is the single source: it generates the str_id_t enum AND the
// two parallel tables in ui_strings.c, so adding a string means one new X(...)
// line and one new STR_ accessor -- no chance of the tables drifting out of
// sync with the enum.
//
// LV_SYMBOL_* icons are still concatenated at the call sites (see pager_task.c,
// ui.c), never baked in here -- this header stays LVGL-free and the icons land
// in the same status line regardless of language.

#define APP_LANG_EN 0
#define APP_LANG_RU 1

// X(NAME, english, russian). The readable form of escaped-byte receipt/emoji
// strings is kept in a /* ... */ comment on the same logical line.
#define UI_STRING_LIST(X) \
    X(WIFI_CONNECTING,    "Connecting to WiFi...",            "Подключение к WiFi...") \
    X(WIFI_FAILED,        "WiFi unavailable, restarting...",  "WiFi недоступен, перезагрузка...") \
    X(TIME_SYNCING,       "Syncing time...",                  "Синхронизация времени...") \
    X(NO_MESSAGES,        "No new messages",                  "Нет новых сообщений") \
    X(NO_TEXT_PLACEHOLDER,"[attachment without text]",        "[вложение без текста]") \
    X(WAITING,            "Waiting for messages",             "Ожидание сообщений") \
    X(WAITING_PROXY,      "Waiting for messages (SOCKS5)",    "Ожидание сообщений (SOCKS5)") \
    X(NEW_MESSAGES_FMT,   "New: %d",                          "Новых: %d") \
    X(NOTHING_TO_ACK,     "Nothing to acknowledge",           "Нечего подтверждать") \
    X(ACK_SENDING,        "Sending \"read\"...",              "Отправляю «прочитано»...") \
    X(ACK_SENT,           "Acknowledged",                     "Подтверждено") \
    X(ACK_FAILED,         "Receipt not delivered",            "Подтверждение не доставлено") \
    X(ACK_QUEUE_FULL,     "Receipt queue full",               "Очередь подтверждений переполнена") \
    X(TELEGRAM_OFFLINE,   "No Telegram connection, retrying...","Нет связи с Telegram, повтор...") \
    X(RECEIPT_READ,       "\xE2\x9C\x85 Read",         "\xE2\x9C\x85 \xD0\x9F\xD1\x80\xD0\xBE\xD1\x87\xD0\xB8\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE") /* "✅ Read" / "✅ Прочитано" */ \
    X(RECEIPT_DELIVERED,  "\xF0\x9F\x93\xA8 Delivered", "\xF0\x9F\x93\xA8 \xD0\x94\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD0\xB2\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xBE") /* "📨 Delivered" / "📨 Доставлено" */ \
    X(CMD_PONG,           "\xF0\x9F\x8F\x93 pong", "\xF0\x9F\x8F\x93 pong") /* "🏓 pong" */ \
    X(CMD_STATUS_TITLE,   "\xF0\x9F\x93\x9F Pager status", "\xF0\x9F\x93\x9F \xD0\xA1\xD0\xBE\xD1\x81\xD1\x82\xD0\xBE\xD1\x8F\xD0\xBD\xD0\xB8\xD0\xB5 \xD0\xBF\xD0\xB5\xD0\xB9\xD0\xB4\xD0\xB6\xD0\xB5\xD1\x80\xD0\xB0") /* "📟 Pager status" / "📟 Состояние пейджера" */ \
    X(CMD_STATUS_FIRMWARE,"Firmware",                        "Прошивка") \
    X(CMD_STATUS_BUILT,   "Built",                            "Собрана") \
    X(CMD_STATUS_UPTIME,  "Uptime",                           "Аптайм") \
    X(CMD_STATUS_WIFI,    "WiFi",                             "WiFi") \
    X(CMD_STATUS_WIFI_FMT,"%s, %d dBm, ch %u",                "%s, %d дБм, канал %u") \
    X(CMD_STATUS_IP,      "IP",                               "IP") \
    X(CMD_STATUS_DISCONNECTED,"disconnected",                 "нет соединения") \
    X(CMD_STATUS_PROXY,   "Proxy",                            "Прокси") \
    X(CMD_STATUS_PROXY_NONE,"direct",                         "напрямую") \
    X(CMD_STATUS_QUEUE,   "Queue",                            "Очередь") \
    X(CMD_STATUS_DROPPED, "dropped",                          "потеряно") \
    X(CMD_STATUS_SCREEN,  "Screen",                           "Экран") \
    X(CMD_STATUS_SCREEN_ON,"on",                              "горит") \
    X(CMD_STATUS_SCREEN_OFF,"off",                            "погашен") \
    X(CMD_STATUS_HEAP,    "Heap",                             "Память") \
    X(CMD_STATUS_HEAP_FMT,"%u KB free, %u KB min",            "%u КБ свободно, минимум %u КБ") \
    X(CMD_STATUS_RESET,   "Last reset",                       "Последний сброс") \
    X(CMD_STATUS_CLOCK,   "Clock",                            "Часы") \
    X(CMD_STATUS_CLOCK_UNSYNCED,"not synced",                 "не синхронизированы") \
    X(CMD_PAGER_USAGE,    "Send /pager followed by the text to show on the pager.", "Отправьте /pager и текст, который нужно показать на пейджере.") \
    X(CMD_ANSWERED,       "Command answered",                 "Команда выполнена") \
    X(CMD_FAILED,         "Command reply not delivered",      "Ответ на команду не доставлен") \
    /* Reactions. Both columns are the same emoji on purpose: a reaction is a
       Telegram token picked from a fixed list, not prose, so there is nothing
       to translate -- and only these two are ever set. Note that "checkmark"
       is NOT in Telegram's allowed reaction set; the closest to "received" it
       offers is the OK hand below. */ \
    X(REACTION_DELIVERED, "\xF0\x9F\x91\x8C", "\xF0\x9F\x91\x8C") /* "👌" */ \
    X(REACTION_READ,      "\xF0\x9F\x91\x80", "\xF0\x9F\x91\x80") /* "👀" */ \
    /* Inline mode ("@thisbot ..." typed in any chat) */ \
    X(INLINE_PAGE_TITLE,  "\xF0\x9F\x93\x9F Send to pager", "\xF0\x9F\x93\x9F \xD0\x9E\xD1\x82\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xB8\xD1\x82\xD1\x8C \xD0\xBD\xD0\xB0 \xD0\xBF\xD0\xB5\xD0\xB9\xD0\xB4\xD0\xB6\xD0\xB5\xD1\x80") /* "📟 Send to pager" / "📟 Отправить на пейджер" */ \
    X(INLINE_STATUS_DESC_FMT,"online · queue %u/%u",         "онлайн · очередь %u/%u") \
    X(INLINE_PING_TITLE,  "\xF0\x9F\x8F\x93 Ping", "\xF0\x9F\x8F\x93 Ping") /* "🏓 Ping" */ \
    X(INLINE_PING_DESC,   "check the link",                   "проверить связь") \
    X(INLINE_PENDING,     "\xE2\x8F\xB3", "\xE2\x8F\xB3") /* "⏳" -- the button label that buys us an inline_message_id */ \
    X(INLINE_PENDING_ALERT,"The pager has not received this yet","Пейджер ещё не получил это сообщение") \
    X(INLINE_ANSWERED,    "Inline request answered",          "Инлайн-запрос обработан") \
    X(INLINE_FAILED,      "Inline request not answered",      "Инлайн-запрос не обработан") \
    /* Provisioning (first-boot captive portal) */ \
    X(PROVISION_TITLE,    "Setup",                            "Настройка") \
    X(PROVISION_AP_LABEL, "1. Join this WiFi:",               "1. Подключитесь к WiFi:") \
    X(PROVISION_URL_LABEL,"2. Open in a browser:",            "2. Откройте в браузере:") \
    X(PROVISION_SAVED,    "Saved. Rebooting...",              "Сохранено. Перезагрузка...") \
    X(PROVISION_BADINPUT, "Check the fields and retry.",      "Проверьте поля и повторите.") \
    X(STA_FAILED_AP,      "WiFi failed, opening setup...",    "WiFi недоступен, открыта настройка...")

// The enum is generated from the same list, so the two tables in ui_strings.c
// cannot drift away from these ids.
typedef enum {
#define X(name, en, ru) STR_ID_##name,
    UI_STRING_LIST(X)
#undef X
    STR_ID__COUNT
} str_id_t;

// Returns the string for `id` in the currently selected language. Bounds-checked
// so a stray id resolves to "" rather than a read past the table end.
const char *ui_str(str_id_t id);

// Selects the active language table. Call once at boot after settings_load().
// Falls back to English for any unknown id.
void ui_set_language(int lang_id);

// Call-site accessors: STR_FOO keeps reading like a literal at the use site,
// even though it now resolves through ui_str(). Because they are function
// calls and not literals, they must NOT be string-literal-concatenated with
// adjacent literals ("foo " STR_BAR) -- format with %s instead, or use
// ui_set_statusf() for status lines that mix a string with an LV_SYMBOL_.
#define STR_WIFI_CONNECTING         ui_str(STR_ID_WIFI_CONNECTING)
#define STR_WIFI_FAILED             ui_str(STR_ID_WIFI_FAILED)
#define STR_TIME_SYNCING            ui_str(STR_ID_TIME_SYNCING)
#define STR_NO_MESSAGES             ui_str(STR_ID_NO_MESSAGES)
#define STR_NO_TEXT_PLACEHOLDER     ui_str(STR_ID_NO_TEXT_PLACEHOLDER)
#define STR_WAITING                 ui_str(STR_ID_WAITING)
#define STR_WAITING_PROXY           ui_str(STR_ID_WAITING_PROXY)
#define STR_NEW_MESSAGES_FMT        ui_str(STR_ID_NEW_MESSAGES_FMT)
#define STR_NOTHING_TO_ACK          ui_str(STR_ID_NOTHING_TO_ACK)
#define STR_ACK_SENDING             ui_str(STR_ID_ACK_SENDING)
#define STR_ACK_SENT                ui_str(STR_ID_ACK_SENT)
#define STR_ACK_FAILED              ui_str(STR_ID_ACK_FAILED)
#define STR_ACK_QUEUE_FULL          ui_str(STR_ID_ACK_QUEUE_FULL)
#define STR_TELEGRAM_OFFLINE        ui_str(STR_ID_TELEGRAM_OFFLINE)
#define STR_RECEIPT_READ            ui_str(STR_ID_RECEIPT_READ)
#define STR_RECEIPT_DELIVERED       ui_str(STR_ID_RECEIPT_DELIVERED)
#define STR_CMD_PONG                ui_str(STR_ID_CMD_PONG)
#define STR_CMD_STATUS_TITLE        ui_str(STR_ID_CMD_STATUS_TITLE)
#define STR_CMD_STATUS_FIRMWARE     ui_str(STR_ID_CMD_STATUS_FIRMWARE)
#define STR_CMD_STATUS_BUILT        ui_str(STR_ID_CMD_STATUS_BUILT)
#define STR_CMD_STATUS_UPTIME       ui_str(STR_ID_CMD_STATUS_UPTIME)
#define STR_CMD_STATUS_WIFI         ui_str(STR_ID_CMD_STATUS_WIFI)
#define STR_CMD_STATUS_WIFI_FMT     ui_str(STR_ID_CMD_STATUS_WIFI_FMT)
#define STR_CMD_STATUS_IP           ui_str(STR_ID_CMD_STATUS_IP)
#define STR_CMD_STATUS_DISCONNECTED ui_str(STR_ID_CMD_STATUS_DISCONNECTED)
#define STR_CMD_STATUS_PROXY        ui_str(STR_ID_CMD_STATUS_PROXY)
#define STR_CMD_STATUS_PROXY_NONE   ui_str(STR_ID_CMD_STATUS_PROXY_NONE)
#define STR_CMD_STATUS_QUEUE        ui_str(STR_ID_CMD_STATUS_QUEUE)
#define STR_CMD_STATUS_DROPPED      ui_str(STR_ID_CMD_STATUS_DROPPED)
#define STR_CMD_STATUS_SCREEN       ui_str(STR_ID_CMD_STATUS_SCREEN)
#define STR_CMD_STATUS_SCREEN_ON    ui_str(STR_ID_CMD_STATUS_SCREEN_ON)
#define STR_CMD_STATUS_SCREEN_OFF   ui_str(STR_ID_CMD_STATUS_SCREEN_OFF)
#define STR_CMD_STATUS_HEAP         ui_str(STR_ID_CMD_STATUS_HEAP)
#define STR_CMD_STATUS_HEAP_FMT     ui_str(STR_ID_CMD_STATUS_HEAP_FMT)
#define STR_CMD_STATUS_RESET        ui_str(STR_ID_CMD_STATUS_RESET)
#define STR_CMD_STATUS_CLOCK        ui_str(STR_ID_CMD_STATUS_CLOCK)
#define STR_CMD_STATUS_CLOCK_UNSYNCED ui_str(STR_ID_CMD_STATUS_CLOCK_UNSYNCED)
#define STR_CMD_PAGER_USAGE         ui_str(STR_ID_CMD_PAGER_USAGE)
#define STR_CMD_ANSWERED            ui_str(STR_ID_CMD_ANSWERED)
#define STR_CMD_FAILED              ui_str(STR_ID_CMD_FAILED)
#define STR_REACTION_DELIVERED      ui_str(STR_ID_REACTION_DELIVERED)
#define STR_REACTION_READ           ui_str(STR_ID_REACTION_READ)
#define STR_INLINE_PAGE_TITLE       ui_str(STR_ID_INLINE_PAGE_TITLE)
#define STR_INLINE_STATUS_DESC_FMT  ui_str(STR_ID_INLINE_STATUS_DESC_FMT)
#define STR_INLINE_PING_TITLE       ui_str(STR_ID_INLINE_PING_TITLE)
#define STR_INLINE_PING_DESC        ui_str(STR_ID_INLINE_PING_DESC)
#define STR_INLINE_PENDING          ui_str(STR_ID_INLINE_PENDING)
#define STR_INLINE_PENDING_ALERT    ui_str(STR_ID_INLINE_PENDING_ALERT)
#define STR_INLINE_ANSWERED         ui_str(STR_ID_INLINE_ANSWERED)
#define STR_INLINE_FAILED           ui_str(STR_ID_INLINE_FAILED)
#define STR_PROVISION_TITLE         ui_str(STR_ID_PROVISION_TITLE)
#define STR_PROVISION_AP_LABEL      ui_str(STR_ID_PROVISION_AP_LABEL)
#define STR_PROVISION_URL_LABEL     ui_str(STR_ID_PROVISION_URL_LABEL)
#define STR_PROVISION_SAVED         ui_str(STR_ID_PROVISION_SAVED)
#define STR_PROVISION_BADINPUT      ui_str(STR_ID_PROVISION_BADINPUT)
#define STR_STA_FAILED_AP           ui_str(STR_ID_STA_FAILED_AP)
