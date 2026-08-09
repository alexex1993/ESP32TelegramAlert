#include "msg_queue.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "msgq";

// Plain ring buffer rather than a FreeRTOS queue: the UI needs to peek at the
// head and report a depth, and overflow has to evict from the front, none of
// which xQueue offers.
static pager_msg_t s_slots[APP_MSG_QUEUE_LEN];
static size_t s_head;    // Index of the oldest message.
static size_t s_count;
static size_t s_dropped;
static SemaphoreHandle_t s_lock;

void msg_queue_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock);
    s_head = 0;
    s_count = 0;
    s_dropped = 0;
}

void msg_queue_push(const pager_msg_t *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_count == APP_MSG_QUEUE_LEN) {
        ESP_LOGW(TAG, "queue full, dropping oldest message %lld",
                 (long long)s_slots[s_head].message_id);
        s_head = (s_head + 1) % APP_MSG_QUEUE_LEN;
        s_count--;
        s_dropped++;
    }

    size_t tail = (s_head + s_count) % APP_MSG_QUEUE_LEN;
    s_slots[tail] = *msg;
    s_count++;

    xSemaphoreGive(s_lock);
}

bool msg_queue_peek(pager_msg_t *out)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count > 0) {
        *out = s_slots[s_head];
        found = true;
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool msg_queue_pop(pager_msg_t *out)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count > 0) {
        *out = s_slots[s_head];
        s_head = (s_head + 1) % APP_MSG_QUEUE_LEN;
        s_count--;
        found = true;
    }
    xSemaphoreGive(s_lock);
    return found;
}

size_t msg_queue_count(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = s_count;
    xSemaphoreGive(s_lock);
    return count;
}

size_t msg_queue_dropped(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t dropped = s_dropped;
    xSemaphoreGive(s_lock);
    return dropped;
}
