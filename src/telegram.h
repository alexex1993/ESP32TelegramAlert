#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "https_client.h"
#include "msg_queue.h"

// Long-polls getUpdates and returns the messages addressed to the configured
// chat. `offset` is read and advanced in place, past every update seen --
// including ones filtered out -- so Telegram stops replaying them.
//
// Returns HTTPS_ERR_ABORTED if abort_fn cut the poll short; that leaves
// `offset` untouched, so nothing is lost and the next poll refetches.
esp_err_t telegram_poll(int64_t *offset, https_abort_fn abort_fn, void *abort_ctx,
                         pager_msg_t *out, int max_out, int *out_count);

// Sends `text` as a reply to `reply_to_message_id` in `chat_id`. If the
// original message is gone the text is still delivered, just unthreaded.
esp_err_t telegram_reply(int64_t chat_id, int64_t reply_to_message_id, const char *text);
