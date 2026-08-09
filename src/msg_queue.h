#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

// One paged message, waiting to be read and acknowledged.
typedef struct {
    int64_t chat_id;
    int64_t message_id;
    int64_t date;                    // Unix seconds, UTC, as sent by Telegram.
    char from[APP_MSG_FROM_MAX];     // Sender's display name, UTF-8.
    char text[APP_MSG_TEXT_MAX];     // Message body, UTF-8, possibly truncated.
} pager_msg_t;

void msg_queue_init(void);

// Appends a message. When the queue is full the oldest entry is discarded to
// make room (and counted in msg_queue_dropped()), because on a pager the
// newest page is the one worth showing.
void msg_queue_push(const pager_msg_t *msg);

// Copies the oldest message without removing it. False if the queue is empty.
bool msg_queue_peek(pager_msg_t *out);

// Removes and copies the oldest message. False if the queue is empty.
bool msg_queue_pop(pager_msg_t *out);

size_t msg_queue_count(void);

// Messages evicted by overflow since boot. These were never shown and can no
// longer be acknowledged, so the UI reports the count.
size_t msg_queue_dropped(void);
