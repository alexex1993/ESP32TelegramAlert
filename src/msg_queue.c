#include "msg_queue.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "msg_store.h"

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

    // Restore the queue from NVS before the pager task can push or the
    // button task can pop. A failed load wipes stale slot blobs so the
    // ring starts truly empty rather than with last firmware's ghosts.
    msg_store_init();
    size_t head = 0, count = 0, dropped = 0;
    if (!msg_store_load(s_slots, APP_MSG_QUEUE_LEN, &head, &count, &dropped)) {
        msg_store_clear(APP_MSG_QUEUE_LEN);
    }
    s_head = head;
    s_count = count;
    s_dropped = dropped;
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

    // Write the slot before bumping meta: if power dies between the two,
    // load() still sees the pre-push count and the orphaned slot blob is
    // simply overwritten by the next push into that index. The opposite
    // order would let meta advertise a slot whose blob never landed.
    msg_store_save_slot(tail, msg);
    msg_store_save_meta(s_head, s_count, s_dropped);

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

bool msg_queue_peek_recent(size_t back, pager_msg_t *out)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (back < s_count) {
        // The newest message sits one slot before the tail; walk backwards
        // from there. back < s_count keeps the subtraction inside the ring.
        size_t index = (s_head + s_count - 1 - back) % APP_MSG_QUEUE_LEN;
        *out = s_slots[index];
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
        // No slot erase: the index now holds stale data, but it sits past
        // (head+count) so load() will not read it, and the next push into
        // that index overwrites both RAM and NVS.
        msg_store_save_meta(s_head, s_count, s_dropped);
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
