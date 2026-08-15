#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

// One paged message, waiting to be read and acknowledged.
typedef struct {
    int64_t chat_id;                 // Zero for a page that arrived inline.
    int64_t message_id;
    int64_t date;                    // Unix seconds, UTC, as sent by Telegram.
    char from[APP_MSG_FROM_MAX];     // Sender's display name, UTF-8.
    char text[APP_MSG_TEXT_MAX];     // Message body, UTF-8, possibly truncated.
    // Set only for pages that arrived through inline mode, where Telegram
    // never tells the bot which chat the message landed in. Everything that
    // reports back -- both receipts -- has to edit the message through this
    // id instead of addressing a chat. Empty for ordinary messages.
    char inline_message_id[APP_INLINE_MSG_ID_MAX];
} pager_msg_t;

// True for a page that came from inline mode, i.e. one that can be reported
// on but not replied to. A page with neither a chat id nor an inline id can
// be shown but never marked; see send_delivery_receipt() in pager_task.c.
static inline bool pager_msg_is_inline(const pager_msg_t *msg)
{
    return msg->inline_message_id[0] != '\0';
}

void msg_queue_init(void);

// Appends a message. When the queue is full the oldest entry is discarded to
// make room (and counted in msg_queue_dropped()), because on a pager the
// newest page is the one worth showing.
void msg_queue_push(const pager_msg_t *msg);

// Copies the oldest message without removing it. False if the queue is empty.
bool msg_queue_peek(pager_msg_t *out);

// Copies the `back`-th newest message without removing it: 0 is the message
// that arrived last, 1 the one before it. False once `back` reaches the depth
// of the queue. Counted from the new end rather than the old one because that
// is the end nothing else moves -- pops take from the front, so an index here
// keeps pointing at the same page for as long as that page is queued.
//
// This is how /last reads the queue out into a chat (see commands.c). It
// copies rather than lending a pointer so nothing outside the mutex ever
// touches a slot.
bool msg_queue_peek_recent(size_t back, pager_msg_t *out);

// Removes and copies the oldest message. False if the queue is empty.
bool msg_queue_pop(pager_msg_t *out);

size_t msg_queue_count(void);

// Messages evicted by overflow since boot. These were never shown and can no
// longer be acknowledged, so the UI reports the count.
size_t msg_queue_dropped(void);
