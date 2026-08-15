#include "commands.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_config.h"
#include "msg_queue.h"
#include "net_conn.h"
#include "screen.h"
#include "settings.h"
#include "telegram.h"
#include "ui_strings.h"
#include "wifi_manager.h"

static const char *TAG = "commands";

// Appends to a buffer that already holds *len bytes and never runs off the
// end; *len is left as the new length so the next call carries on from there.
static void append(char *buf, size_t size, size_t *len, const char *fmt, ...)
{
    if (*len + 1 >= size) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf + *len, size - *len, fmt, ap);
    va_end(ap);

    if (written < 0) {
        return;
    }
    // vsnprintf returns what it *would* have written, so clamp on truncation.
    *len = ((size_t)written >= size - *len) ? size - 1 : *len + (size_t)written;
}

// Returns whatever follows "/name" in the message, with the whitespace that
// separates them skipped -- so "" for a bare command and the argument for one
// that carries anything. NULL when the text is not this command at all.
//
// Accepts Telegram's "/name@thisbot" form (which is what a command sent in a
// group looks like). Case-insensitive because phone keyboards like to
// capitalise the first letter. The result points into `text`, so it lives
// exactly as long as the message does.
static const char *command_arg(const char *text, const char *name)
{
    if (text[0] != '/') {
        return NULL;
    }

    size_t n = strlen(name);
    if (strncasecmp(text + 1, name, n) != 0) {
        return NULL;
    }

    const char *rest = text + 1 + n;
    if (*rest == '@') {
        rest++;
        while (*rest != '\0' && *rest != ' ' && *rest != '\n') {
            rest++;
        }
    }
    // Without this "/statuses" would answer as "/status".
    if (*rest != '\0' && *rest != ' ' && *rest != '\n') {
        return NULL;
    }

    // A newline counts as the separator too: "/pager" on its own line with the
    // text under it is a perfectly ordinary way to send a multi-line page.
    while (*rest == ' ' || *rest == '\n') {
        rest++;
    }
    return rest;
}

static bool is_command(const char *text, const char *name)
{
    return command_arg(text, name) != NULL;
}

// Deliberately not translated: these are technical identifiers, read the same
// way as esp_err_to_name() output in the log, and searchable in the ESP-IDF
// docs. A reset reason is the first thing worth knowing when a pager that
// should have been up for weeks reports an uptime of two minutes.
static const char *reset_reason_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "UNKNOWN";
    }
}

static void format_uptime(char *buf, size_t size)
{
    int64_t total = esp_timer_get_time() / 1000000;
    int days = (int)(total / 86400);
    int hours = (int)(total / 3600 % 24);
    int minutes = (int)(total / 60 % 60);
    int seconds = (int)(total % 60);

    if (days > 0) {
        snprintf(buf, size, "%dd %02d:%02d:%02d", days, hours, minutes, seconds);
    } else {
        snprintf(buf, size, "%02d:%02d:%02d", hours, minutes, seconds);
    }
}

// The device clock runs on UTC and TZ_OFFSET_HOURS is applied where it is
// shown, exactly as ui.c does for the [HH:MM] stamp. The offset is a runtime
// setting now, read through settings_get().
static void format_clock(char *buf, size_t size)
{
    time_t now = time(NULL);
    // SNTP is best-effort (see sync_time() in main.c) and nothing depends on
    // it, so an unsynced clock is a normal state to report rather than hide.
    // Anything before 2024 means the answer never came and this is uptime.
    if (now < 1704067200) {
        snprintf(buf, size, "%s", STR_CMD_STATUS_CLOCK_UNSYNCED);
        return;
    }

    int tz = settings_get()->tz_offset_hours;
    time_t shifted = now + (time_t)tz * 3600;
    struct tm tm;
    gmtime_r(&shifted, &tm);
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d UTC%+d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
             tz);
}

void commands_build_status(char *buf, size_t size)
{
    size_t len = 0;
    buf[0] = '\0';

    const esp_app_desc_t *app = esp_app_get_description();
    append(buf, size, &len, "%s\n", STR_CMD_STATUS_TITLE);
    append(buf, size, &len, "%s: %s (ESP-IDF %s)\n", STR_CMD_STATUS_FIRMWARE,
           app->version, app->idf_ver);
    append(buf, size, &len, "%s: %s %s\n", STR_CMD_STATUS_BUILT, app->date, app->time);

    char scratch[40];
    format_uptime(scratch, sizeof(scratch));
    append(buf, size, &len, "%s: %s\n", STR_CMD_STATUS_UPTIME, scratch);

    wifi_manager_info_t wifi;
    wifi_manager_get_info(&wifi);
    if (wifi.connected) {
        // Split into label + translated format: STR_* are runtime lookups now
        // and cannot be string-literal-concatenated with adjacent text.
        append(buf, size, &len, "%s: ", STR_CMD_STATUS_WIFI);
        append(buf, size, &len, STR_CMD_STATUS_WIFI_FMT, wifi.ssid, wifi.rssi, wifi.channel);
        append(buf, size, &len, "\n");
        append(buf, size, &len, "%s: %s\n", STR_CMD_STATUS_IP, wifi.ip);
    } else {
        // The reply got out somehow, so this is a race with a reconnect rather
        // than a contradiction -- still worth saying which half is missing.
        append(buf, size, &len, "%s: %s\n", STR_CMD_STATUS_WIFI, STR_CMD_STATUS_DISCONNECTED);
    }

    // Only whether a proxy is in use, not which one: the reply goes to whoever
    // asked, and the endpoint is the operator's business, not the caller's.
    append(buf, size, &len, "%s: %s\n", STR_CMD_STATUS_PROXY,
           net_conn_uses_proxy() ? "SOCKS5" : STR_CMD_STATUS_PROXY_NONE);

    append(buf, size, &len, "%s: %u/%u", STR_CMD_STATUS_QUEUE,
           (unsigned)msg_queue_count(), (unsigned)APP_MSG_QUEUE_LEN);
    size_t dropped = msg_queue_dropped();
    if (dropped > 0) {
        // Same warning the header carries: these can never be acknowledged.
        append(buf, size, &len, ", %s %u", STR_CMD_STATUS_DROPPED, (unsigned)dropped);
    }
    append(buf, size, &len, "\n");

    append(buf, size, &len, "%s: %s\n", STR_CMD_STATUS_SCREEN,
           screen_is_on() ? STR_CMD_STATUS_SCREEN_ON : STR_CMD_STATUS_SCREEN_OFF);
    append(buf, size, &len, "%s: ", STR_CMD_STATUS_HEAP);
    append(buf, size, &len, STR_CMD_STATUS_HEAP_FMT,
           (unsigned)(esp_get_free_heap_size() / 1024),
           (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    append(buf, size, &len, "\n");
    append(buf, size, &len, "%s: %s\n", STR_CMD_STATUS_RESET, reset_reason_name());

    format_clock(scratch, sizeof(scratch));
    append(buf, size, &len, "%s: %s", STR_CMD_STATUS_CLOCK, scratch);
}

command_result_t commands_try_handle(pager_msg_t *msg)
{
    // Static for the same reason pager_task's poll batch is: only that task
    // calls this, and its stack has to hold the TLS handshake that the reply
    // below runs while this buffer is still live.
    static char s_status[COMMANDS_STATUS_MAX];

    // "/pager <text>" is the one command that becomes a page instead of an
    // answer: it strips its own prefix in place and reports COMMAND_NONE, so
    // what reaches the queue, the glass, the SD card and the receipt is <text>
    // alone -- an ordinary page from whoever sent it, cleared by an ordinary
    // button press.
    //
    // That is what makes the pager reachable from a group at all. A bot with
    // privacy mode on -- BotFather's default -- is only shown messages that
    // address it, so a plain line of text in a group never arrives here, while
    // a command always does.
    //
    // Ahead of the chat-id guard below on purpose: the rewrite needs no chat
    // to answer in, so a page sent inline may carry the prefix too.
    const char *page = command_arg(msg->text, "pager");
    if (page && *page != '\0') {
        // memmove, not strcpy: `page` points into the buffer being written.
        memmove(msg->text, page, strlen(page) + 1);
        return COMMAND_NONE;
    }

    // A page that arrived inline has no chat to answer in, and a command that
    // cannot be answered must not be swallowed on top of that -- so "@thisbot
    // /status" sent as a page is shown on the glass like any other text.
    // Tested on the chat id rather than the inline id because that is what the
    // reply below actually needs, and an inline page never has one.
    if (msg->chat_id == 0) {
        return COMMAND_NONE;
    }

    const char *reply;
    if (page) {
        // "/pager" with nothing after it. Answering with what to do beats
        // paging an empty page that someone then has to press the key to
        // clear.
        reply = STR_CMD_PAGER_USAGE;
    } else if (is_command(msg->text, "ping")) {
        reply = STR_CMD_PONG;
    } else if (is_command(msg->text, "status")) {
        commands_build_status(s_status, sizeof(s_status));
        reply = s_status;
    } else {
        return COMMAND_NONE;
    }

    ESP_LOGI(TAG, "answering '%s' from chat %lld", msg->text, (long long)msg->chat_id);
    esp_err_t err = telegram_reply(msg->chat_id, msg->message_id, reply);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reply failed: %s", esp_err_to_name(err));
        return COMMAND_FAILED;
    }
    return COMMAND_ANSWERED;
}
