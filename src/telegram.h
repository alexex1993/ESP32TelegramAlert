#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "app_config.h"
#include "https_client.h"
#include "msg_queue.h"

// result_id of the "send to pager" card. A chosen_inline_result carrying it
// becomes a page; every other one is ignored, because the status and ping
// cards are answers about the device, not messages for it. inline_mode.c
// builds the card with this same id.
#define TELEGRAM_INLINE_RESULT_PAGE "page"

// Someone typing "@thisbot ..." in a chat, anywhere, member or not. Telegram
// expects an answer within seconds and discards a late one.
typedef struct {
    char id[APP_INLINE_ID_MAX];
    char text[APP_INLINE_QUERY_MAX];  // Whatever followed the bot name.
    int64_t from_id;                  // Who is typing; see dedupe_queries().
} telegram_inline_query_t;

// A tap on the "pending" button under an inline page. The bot has exactly one
// button, so every callback is that one and only needs dismissing.
typedef struct {
    char id[APP_INLINE_ID_MAX];
} telegram_callback_t;

// What one getUpdates lap produced, split by what has to happen to it. The
// caller owns all three arrays and fills the *_max fields; telegram_poll()
// fills the *_count fields and never writes past a max.
typedef struct {
    pager_msg_t *msgs;
    int msgs_max;
    int msgs_count;
    telegram_inline_query_t *queries;
    int queries_max;
    int queries_count;
    telegram_callback_t *callbacks;
    int callbacks_max;
    int callbacks_count;
} telegram_batch_t;

// One card offered in answer to an inline query.
typedef struct {
    const char *id;           // result_id, echoed back in chosen_inline_result.
    const char *title;
    const char *description;  // Second line under the title; may be NULL.
    const char *text;         // What is posted into the chat when picked.
    // Attaches the "pending" button. Telegram hands back an inline_message_id
    // only for results that carry an inline keyboard, and without one there is
    // no handle to report "delivered" or "read" through -- the button is the
    // price of a receipt, not decoration.
    bool track;
} telegram_inline_result_t;

// Long-polls getUpdates and sorts the batch. `offset` is read and advanced in
// place, past every update seen -- including ones filtered out -- so Telegram
// stops replaying them.
//
// Returns HTTPS_ERR_ABORTED if abort_fn cut the poll short; that leaves
// `offset` untouched, so nothing is lost and the next poll refetches.
esp_err_t telegram_poll(int64_t *offset, https_abort_fn abort_fn, void *abort_ctx,
                        telegram_batch_t *batch);

// Sends `text` as a reply to `reply_to_message_id` in `chat_id`. If the
// original message is gone the text is still delivered, just unthreaded.
esp_err_t telegram_reply(int64_t chat_id, int64_t reply_to_message_id, const char *text);

// Sends `text` into `chat_id` on its own, threaded under nothing. This is for
// what the device says without being asked -- the boot announcement to the
// contact list; everything else here answers a message and replies to it.
esp_err_t telegram_send_message(int64_t chat_id, const char *text);

// Sets the bot's single reaction on a message, replacing whatever it set
// before -- which is how "read" supersedes "delivered". Channels forbid bot
// reactions and any chat can turn them off, so a failure here is an ordinary
// outcome and callers fall back to telegram_reply().
esp_err_t telegram_set_reaction(int64_t chat_id, int64_t message_id, const char *emoji);

// Answers an inline query with `count` cards.
esp_err_t telegram_answer_inline_query(const char *query_id,
                                       const telegram_inline_result_t *results, int count);

// Dismisses the spinner on a tapped inline button, with a toast if `text` is
// not NULL. Telegram drops the answer once the query has expired.
esp_err_t telegram_answer_callback(const char *callback_id, const char *text);

// Replaces the whole text of a message posted through inline mode, and drops
// its keyboard along with it. This is the only way to report back into a chat
// whose id the bot never learns -- setMessageReaction takes a chat id and has
// no inline form, so an inline page cannot be reacted to.
esp_err_t telegram_edit_inline_text(const char *inline_message_id, const char *text);
