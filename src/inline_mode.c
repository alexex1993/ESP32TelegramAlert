#include "inline_mode.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "app_config.h"
#include "commands.h"
#include "msg_queue.h"
#include "ui_strings.h"

static const char *TAG = "inline";

bool inline_answer_query(const telegram_inline_query_t *query)
{
    // Static for the same reason commands.c keeps its reply buffer static: the
    // TLS handshake for the answer runs on the pager task's stack while this
    // is still live, and 640 bytes there is 640 bytes not available to it.
    static char s_status[COMMANDS_STATUS_MAX];

    // Telegram hands over exactly what follows the bot name, so a query is
    // blank the moment the user has typed the name and a space -- which is the
    // state the status cards are for.
    const char *text = query->text;
    while (*text == ' ' || *text == '\n') {
        text++;
    }

    telegram_inline_result_t results[2];
    int count = 0;
    char description[64];

    if (*text == '\0') {
        commands_build_status(s_status, sizeof(s_status));
        snprintf(description, sizeof(description), STR_INLINE_STATUS_DESC_FMT,
                 (unsigned)msg_queue_count(), (unsigned)APP_MSG_QUEUE_LEN);

        results[count++] = (telegram_inline_result_t){
            .id = "status",
            .title = STR_CMD_STATUS_TITLE,
            .description = description,
            .text = s_status,
        };
        results[count++] = (telegram_inline_result_t){
            .id = "ping",
            .title = STR_INLINE_PING_TITLE,
            .description = STR_INLINE_PING_DESC,
            .text = STR_CMD_PONG,
        };
    } else {
        // The typed text is both the preview under the title and the message
        // that gets posted: what you see in the list is what lands in the chat
        // and on the glass. `track` is what makes a receipt possible at all.
        results[count++] = (telegram_inline_result_t){
            .id = TELEGRAM_INLINE_RESULT_PAGE,
            .title = STR_INLINE_PAGE_TITLE,
            .description = text,
            .text = text,
            .track = true,
        };
    }

    esp_err_t err = telegram_answer_inline_query(query->id, results, count);
    if (err != ESP_OK) {
        // Routine rather than alarming: an answer that took longer than
        // Telegram's window is refused, and on a link slow enough for that the
        // next keystroke has already superseded this query anyway.
        ESP_LOGW(TAG, "answering query %s failed: %s", query->id, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "answered inline query %s with %d card(s)", query->id, count);
    return true;
}
