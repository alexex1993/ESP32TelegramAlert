#include "telegram.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"

#include "app_config.h"
#include "settings.h"
#include "ui_strings.h"

static const char *TAG = "telegram";

// "/bot" + token + longest method name.
#define TELEGRAM_PATH_MAX 128

// Every call except the long poll is a short round trip: connect, hand over a
// few hundred bytes, read the acknowledgement.
#define TELEGRAM_CALL_TIMEOUT_MS (APP_HTTP_CONNECT_TIMEOUT_MS + 10000)

// Inline pages carry no message id of their own, but the queue, the UI and
// the SD log all key a page on one. Hand out a distinct negative id per page,
// which no real Telegram message id can collide with. It restarts at zero on
// reboot; the UI identity also compares inline_message_id, so a restored page
// and a fresh one still read as different pages.
static int64_t s_inline_seq;

static void build_path(char *buf, size_t size, const char *method)
{
    snprintf(buf, size, "/bot%s/%s", settings_get()->bot_token, method);
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

// An ordinary chat message. False when it cannot be paged at all.
static bool parse_message(const cJSON *message, pager_msg_t *msg)
{
    const cJSON *chat = cJSON_GetObjectItemCaseSensitive(message, "chat");
    int64_t chat_id = chat ? json_int64(chat, "id") : 0;
    // There is no allow-list: whoever finds the bot gets paged, and the
    // receipts go back to the chat the message came from. A message with
    // no chat id is the one exception -- it could be shown but never
    // answered, since a chat receipt is addressed by chat id.
    if (chat_id == 0) {
        return false;
    }

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
    return true;
}

// A card someone picked out of an inline answer. Only the page card becomes a
// message: the status and ping cards report on the device, so acting on them
// here would page it with the text it just printed about itself.
static bool parse_chosen_inline(const cJSON *chosen, pager_msg_t *msg)
{
    const char *result_id = json_string(chosen, "result_id");
    if (!result_id || strcmp(result_id, TELEGRAM_INLINE_RESULT_PAGE) != 0) {
        return false;
    }

    memset(msg, 0, sizeof(*msg));
    msg->chat_id = 0;  // Inline mode never tells the bot where it landed.
    msg->message_id = --s_inline_seq;
    // ChosenInlineResult carries no date -- unlike a Message, it is an event
    // rather than a thing with a send time. The device clock is the best
    // stamp available, and it is the same one the SD log would use anyway.
    msg->date = (int64_t)time(NULL);
    fill_sender(msg, chosen);
    copy_utf8(msg->text, sizeof(msg->text), json_string(chosen, "query"));
    copy_utf8(msg->inline_message_id, sizeof(msg->inline_message_id),
              json_string(chosen, "inline_message_id"));

    if (msg->inline_message_id[0] == '\0') {
        // The page still shows -- it just can never be marked, because the
        // keyboard that would have bought us the id did not come back.
        ESP_LOGW(TAG, "chosen inline result has no inline_message_id, page cannot be marked");
    }
    if (msg->text[0] == '\0') {
        copy_utf8(msg->text, sizeof(msg->text), STR_NO_TEXT_PLACEHOLDER);
    }
    return true;
}

static bool parse_inline_query(const cJSON *query, telegram_inline_query_t *out)
{
    const char *id = json_string(query, "id");
    if (!id) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    copy_utf8(out->id, sizeof(out->id), id);
    copy_utf8(out->text, sizeof(out->text), json_string(query, "query"));

    const cJSON *from = cJSON_GetObjectItemCaseSensitive(query, "from");
    out->from_id = from ? json_int64(from, "id") : 0;
    return true;
}

esp_err_t telegram_poll(int64_t *offset, https_abort_fn abort_fn, void *abort_ctx,
                         telegram_batch_t *batch)
{
    batch->msgs_count = 0;
    batch->queries_count = 0;
    batch->callbacks_count = 0;

    // Every type listed here has a handler below; anything not listed is not
    // even delivered, which keeps a bot with other features enabled from
    // spending this device's poll budget on updates it would only skip.
    char body[256];
    snprintf(body, sizeof(body),
             "{\"offset\":%lld,\"timeout\":%d,\"limit\":%d,\"allowed_updates\":"
             "[\"message\",\"inline_query\",\"chosen_inline_result\",\"callback_query\"]}",
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

        const cJSON *item;
        if (cJSON_IsObject(item = cJSON_GetObjectItemCaseSensitive(update, "message"))) {
            if (batch->msgs_count >= batch->msgs_max) {
                ESP_LOGW(TAG, "more messages than output slots, rest will be refetched");
                break;
            }
            if (parse_message(item, &batch->msgs[batch->msgs_count])) {
                batch->msgs_count++;
            } else {
                ESP_LOGW(TAG, "update %lld carries no chat id, dropped", (long long)update_id);
            }
        } else if (cJSON_IsObject(item = cJSON_GetObjectItemCaseSensitive(update, "inline_query"))) {
            if (batch->queries_count >= batch->queries_max) {
                ESP_LOGW(TAG, "more inline queries than slots, rest dropped");
                continue;
            }
            if (parse_inline_query(item, &batch->queries[batch->queries_count])) {
                batch->queries_count++;
            }
        } else if (cJSON_IsObject(item = cJSON_GetObjectItemCaseSensitive(update, "chosen_inline_result"))) {
            if (batch->msgs_count >= batch->msgs_max) {
                ESP_LOGW(TAG, "more messages than output slots, rest will be refetched");
                break;
            }
            if (parse_chosen_inline(item, &batch->msgs[batch->msgs_count])) {
                batch->msgs_count++;
            }
        } else if (cJSON_IsObject(item = cJSON_GetObjectItemCaseSensitive(update, "callback_query"))) {
            const char *id = json_string(item, "id");
            if (!id || batch->callbacks_count >= batch->callbacks_max) {
                continue;
            }
            telegram_callback_t *cb = &batch->callbacks[batch->callbacks_count++];
            memset(cb, 0, sizeof(*cb));
            copy_utf8(cb->id, sizeof(cb->id), id);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// Serialises `payload`, posts it and throws the answer away -- which is what
// every write method here wants, since the only thing worth knowing is whether
// the API said ok. Takes ownership of `payload` on every path.
static esp_err_t post_payload(const char *method, cJSON *payload)
{
    char *body = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    cJSON *root = call_api(method, body, TELEGRAM_CALL_TIMEOUT_MS, NULL, NULL, &err);
    free(body);
    if (!root) {
        return err;
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

    return post_payload("sendMessage", payload);
}

esp_err_t telegram_set_reaction(int64_t chat_id, int64_t message_id, const char *emoji)
{
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(payload, "chat_id", (double)chat_id);
    cJSON_AddNumberToObject(payload, "message_id", (double)message_id);

    // A list, but never more than one entry: bots are treated as non-premium
    // users and may hold a single reaction per message. Setting the next one
    // replaces the last, which is exactly the delivered -> read progression.
    cJSON *list = cJSON_AddArrayToObject(payload, "reaction");
    cJSON *entry = cJSON_CreateObject();
    if (!list || !entry) {
        cJSON_Delete(entry);
        cJSON_Delete(payload);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(entry, "type", "emoji");
    cJSON_AddStringToObject(entry, "emoji", emoji);
    cJSON_AddItemToArray(list, entry);

    return post_payload("setMessageReaction", payload);
}

esp_err_t telegram_answer_inline_query(const char *query_id,
                                        const telegram_inline_result_t *results, int count)
{
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(payload, "inline_query_id", query_id);
    cJSON_AddNumberToObject(payload, "cache_time", APP_INLINE_CACHE_TIME_S);
    // The status card describes this device to the person who asked, so it
    // must not be served out of a cache shared with anyone else.
    cJSON_AddBoolToObject(payload, "is_personal", true);

    cJSON *array = cJSON_AddArrayToObject(payload, "results");
    if (!array) {
        cJSON_Delete(payload);
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < count; i++) {
        const telegram_inline_result_t *r = &results[i];
        cJSON *card = cJSON_CreateObject();
        cJSON *content = cJSON_CreateObject();
        if (!card || !content) {
            cJSON_Delete(card);
            cJSON_Delete(content);
            cJSON_Delete(payload);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(card, "type", "article");
        cJSON_AddStringToObject(card, "id", r->id);
        cJSON_AddStringToObject(card, "title", r->title);
        if (r->description) {
            cJSON_AddStringToObject(card, "description", r->description);
        }
        cJSON_AddStringToObject(content, "message_text", r->text);
        cJSON_AddItemToObject(card, "input_message_content", content);

        if (r->track) {
            // One button, and its only job is to exist: a result without an
            // inline keyboard comes back with no inline_message_id, and then
            // the page could never be marked delivered or read. Both marks
            // edit the message and drop the keyboard with it, so it is on
            // screen only while the page is genuinely in flight.
            cJSON *button = cJSON_CreateObject();
            cJSON *row = cJSON_CreateArray();
            cJSON *keyboard = cJSON_CreateArray();
            cJSON *markup = cJSON_CreateObject();
            if (!button || !row || !keyboard || !markup) {
                cJSON_Delete(button);
                cJSON_Delete(row);
                cJSON_Delete(keyboard);
                cJSON_Delete(markup);
                cJSON_Delete(card);
                cJSON_Delete(payload);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(button, "text", STR_INLINE_PENDING);
            // Tapping it is answered with a toast (see telegram_answer_
            // callback); the payload itself is never read, since this is the
            // only button the firmware ever draws.
            cJSON_AddStringToObject(button, "callback_data", "pending");
            cJSON_AddItemToArray(row, button);
            cJSON_AddItemToArray(keyboard, row);
            cJSON_AddItemToObject(markup, "inline_keyboard", keyboard);
            cJSON_AddItemToObject(card, "reply_markup", markup);
        }

        cJSON_AddItemToArray(array, card);
    }

    return post_payload("answerInlineQuery", payload);
}

esp_err_t telegram_answer_callback(const char *callback_id, const char *text)
{
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(payload, "callback_query_id", callback_id);
    if (text) {
        cJSON_AddStringToObject(payload, "text", text);
    }
    return post_payload("answerCallbackQuery", payload);
}

esp_err_t telegram_edit_inline_text(const char *inline_message_id, const char *text)
{
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(payload, "inline_message_id", inline_message_id);
    cJSON_AddStringToObject(payload, "text", text);
    // No reply_markup: leaving it out is what removes the pending button, so
    // the mark this edit carries is the last word on the message.
    return post_payload("editMessageText", payload);
}
