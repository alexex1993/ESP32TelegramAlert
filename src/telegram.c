#include "telegram.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "app_config.h"
#include "secrets.h"
#include "ui_strings.h"

static const char *TAG = "telegram";

// "/bot" + token + longest method name.
#define TELEGRAM_PATH_MAX 128

static void build_path(char *buf, size_t size, const char *method)
{
    snprintf(buf, size, "/bot%s/%s", SECRET_BOT_TOKEN, method);
}

// Copies a UTF-8 string, truncating on a character boundary rather than in
// the middle of a multi-byte sequence -- which for Cyrillic text is every
// other byte, and would render as a replacement glyph.
static void copy_utf8(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1;
        // Continuation bytes are 10xxxxxx; step back off them to reach the
        // start of the character that got cut.
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int64_t json_int64(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) {
        return 0;
    }
    // Telegram ids stay well inside a double's exact integer range, so this
    // round-trips losslessly.
    return (int64_t)item->valuedouble;
}

static const char *json_string(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

// Returns a parsed response body, or NULL after logging why not. Callers own
// the returned cJSON and must delete it.
static cJSON *call_api(const char *method, const char *json_body, int timeout_ms,
                        https_abort_fn abort_fn, void *abort_ctx, esp_err_t *out_err)
{
    char path[TELEGRAM_PATH_MAX];
    build_path(path, sizeof(path), method);

    https_request_t req = {
        .host = TELEGRAM_API_HOST,
        .port = TELEGRAM_API_PORT,
        .path = path,
        .json_body = json_body,
        .timeout_ms = timeout_ms,
        .abort_fn = abort_fn,
        .abort_ctx = abort_ctx,
    };

    int status = 0;
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = https_post_json(&req, &status, &body, &body_len);
    *out_err = err;
    if (err != ESP_OK) {
        if (err != HTTPS_ERR_ABORTED) {
            ESP_LOGE(TAG, "%s: transport error %s", method, esp_err_to_name(err));
        }
        return NULL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGE(TAG, "%s: response is not valid JSON", method);
        *out_err = ESP_ERR_INVALID_RESPONSE;
        return NULL;
    }

    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) {
        const char *desc = json_string(root, "description");
        ESP_LOGE(TAG, "%s: API returned HTTP %d: %s", method, status,
                 desc ? desc : "(no description)");
        cJSON_Delete(root);
        *out_err = ESP_FAIL;
        return NULL;
    }

    return root;
}

static void fill_sender(pager_msg_t *msg, const cJSON *message)
{
    const cJSON *from = cJSON_GetObjectItemCaseSensitive(message, "from");
    const char *first = from ? json_string(from, "first_name") : NULL;
    const char *last = from ? json_string(from, "last_name") : NULL;

    if (first && last) {
        char joined[APP_MSG_FROM_MAX * 2];
        snprintf(joined, sizeof(joined), "%s %s", first, last);
        copy_utf8(msg->from, sizeof(msg->from), joined);
    } else if (first) {
        copy_utf8(msg->from, sizeof(msg->from), first);
    } else {
        copy_utf8(msg->from, sizeof(msg->from), "?");
    }
}

esp_err_t telegram_poll(int64_t *offset, https_abort_fn abort_fn, void *abort_ctx,
                         pager_msg_t *out, int max_out, int *out_count)
{
    *out_count = 0;

    char body[160];
    snprintf(body, sizeof(body),
             "{\"offset\":%lld,\"timeout\":%d,\"limit\":%d,\"allowed_updates\":[\"message\"]}",
             (long long)*offset, APP_LONGPOLL_TIMEOUT_S, APP_UPDATES_PER_POLL);

    esp_err_t err = ESP_OK;
    cJSON *root = call_api("getUpdates", body, APP_HTTP_SOCKET_TIMEOUT_MS,
                            abort_fn, abort_ctx, &err);
    if (!root) {
        return err;
    }

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsArray(result)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *update = NULL;
    cJSON_ArrayForEach(update, result) {
        int64_t update_id = json_int64(update, "update_id");
        // Advance past every update, even the ones dropped below: an offset
        // that stalls makes Telegram replay the same batch forever.
        if (update_id >= *offset) {
            *offset = update_id + 1;
        }

        const cJSON *message = cJSON_GetObjectItemCaseSensitive(update, "message");
        if (!cJSON_IsObject(message)) {
            continue;
        }

        const cJSON *chat = cJSON_GetObjectItemCaseSensitive(message, "chat");
        int64_t chat_id = chat ? json_int64(chat, "id") : 0;
        // There is no allow-list: whoever finds the bot gets paged, and the
        // receipts go back to the chat the message came from. A message with
        // no chat id is the one exception -- it could be shown but never
        // answered, since both receipts are addressed by chat id.
        if (chat_id == 0) {
            ESP_LOGW(TAG, "update %lld carries no chat id, dropped", (long long)update_id);
            continue;
        }

        if (*out_count >= max_out) {
            // Cannot happen while limit <= max_out, but never overrun.
            ESP_LOGW(TAG, "more updates than output slots, rest will be refetched");
            break;
        }

        pager_msg_t *msg = &out[*out_count];
        memset(msg, 0, sizeof(*msg));
        msg->chat_id = chat_id;
        msg->message_id = json_int64(message, "message_id");
        msg->date = json_int64(message, "date");
        fill_sender(msg, message);

        const char *text = json_string(message, "text");
        if (!text) {
            text = json_string(message, "caption");
        }
        copy_utf8(msg->text, sizeof(msg->text), text ? text : STR_NO_TEXT_PLACEHOLDER);

        (*out_count)++;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t telegram_reply(int64_t chat_id, int64_t reply_to_message_id, const char *text)
{
    // Built with cJSON rather than snprintf so the text is escaped properly --
    // it is arbitrary user input and will contain quotes and newlines.
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(payload, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(payload, "text", text);
    cJSON_AddNumberToObject(payload, "reply_to_message_id", (double)reply_to_message_id);
    // Without this, replying to a deleted message is a hard error and the
    // receipt is lost entirely.
    cJSON_AddBoolToObject(payload, "allow_sending_without_reply", true);

    char *body = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    cJSON *root = call_api("sendMessage", body, APP_HTTP_CONNECT_TIMEOUT_MS + 10000,
                            NULL, NULL, &err);
    free(body);
    if (!root) {
        return err;
    }
    cJSON_Delete(root);
    return ESP_OK;
}
