#include "msg_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"

static const char *TAG = "msgstore";

static const char *NVS_NAMESPACE = "pager";
static const char *KEY_META = "meta";

// Bumped whenever the on-flash layout of meta or a slot changes. A mismatch
// makes load() report empty so the caller wipes stale slot blobs and starts
// clean rather than decoding last firmware's struct padding.
// v2 added pager_msg_t.inline_message_id. The per-slot size check below would
// have caught the growth on its own, but the version is what says so out loud.
#define MSG_STORE_VERSION  2
// "PAG1" --Pager Async Gatekeeper v1-- in little-endian. Just a guard
// against reading random flash as if it were a meta blob.
#define MSG_STORE_MAGIC    0x50414731u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t head;
    uint32_t count;
    uint32_t dropped;
} msg_store_meta_t;

static nvs_handle_t s_handle;

// NVS keys are max 15 chars; "s%03u" -> "s000".."s031" is plenty. The modulo
// is not a guard on a real index -- every caller passes one below
// APP_MSG_QUEUE_LEN -- it is what tells the compiler the number is three
// digits, which at -Os it otherwise assumes could be ten (-Wformat-truncation).
static void slot_key(size_t idx, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "s%03u", (unsigned)idx % 1000u);
}

void msg_store_init(void)
{
    if (s_handle) {
        return;
    }
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(\"%s\") failed: %s", NVS_NAMESPACE, esp_err_to_name(err));
        configASSERT(false);
    }
}

bool msg_store_load(pager_msg_t *slots, size_t capacity,
                    size_t *out_head, size_t *out_count, size_t *out_dropped)
{
    msg_store_meta_t meta;
    size_t len = sizeof(meta);
    esp_err_t err = nvs_get_blob(s_handle, KEY_META, &meta, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no persisted queue yet, starting empty");
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "meta read failed: %s -- starting empty", esp_err_to_name(err));
        return false;
    }
    if (len != sizeof(meta) || meta.magic != MSG_STORE_MAGIC ||
        meta.version != MSG_STORE_VERSION) {
        ESP_LOGW(TAG, "meta magic/version mismatch -- starting empty");
        return false;
    }
    if (meta.head >= capacity || meta.count > capacity) {
        // head must index a real slot, and count cannot exceed the ring.
        // (head+count wrapping is fine, but each individual index is in range.)
        ESP_LOGW(TAG, "meta out of range (head=%u count=%u cap=%u) -- wiping",
                 (unsigned)meta.head, (unsigned)meta.count, (unsigned)capacity);
        return false;
    }

    // Walk the live slots in queue order: [head, head+count) mod capacity.
    // If any slot is missing or its blob size disagrees with the current
    // sizeof(pager_msg_t) (e.g. APP_MSG_TEXT_MAX changed), truncate the
    // queue to what did load. Losing a tail message is preferable to
    // paging a half-decoded one.
    for (size_t i = 0; i < meta.count; i++) {
        size_t idx = (meta.head + i) % capacity;
        char key[8];
        slot_key(idx, key, sizeof(key));

        size_t slen = sizeof(pager_msg_t);
        err = nvs_get_blob(s_handle, key, &slots[idx], &slen);
        if (err != ESP_OK || slen != sizeof(pager_msg_t)) {
            ESP_LOGW(TAG, "slot %u unreadable (%s) -- truncating queue to %u",
                     (unsigned)idx, esp_err_to_name(err), (unsigned)i);
            meta.count = i;
            break;
        }
    }

    *out_head = meta.head;
    *out_count = meta.count;
    *out_dropped = meta.dropped;
    ESP_LOGI(TAG, "restored %u message(s) from NVS", (unsigned)meta.count);
    return true;
}

void msg_store_save_slot(size_t idx, const pager_msg_t *msg)
{
    char key[8];
    slot_key(idx, key, sizeof(key));
    esp_err_t err = nvs_set_blob(s_handle, key, msg, sizeof(*msg));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save slot %u failed: %s", (unsigned)idx, esp_err_to_name(err));
    }
}

void msg_store_save_meta(size_t head, size_t count, size_t dropped)
{
    msg_store_meta_t meta = {
        .magic = MSG_STORE_MAGIC,
        .version = MSG_STORE_VERSION,
        .head = (uint32_t)head,
        .count = (uint32_t)count,
        .dropped = (uint32_t)dropped,
    };
    esp_err_t err = nvs_set_blob(s_handle, KEY_META, &meta, sizeof(meta));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save meta failed: %s", esp_err_to_name(err));
        return;
    }
    // nvs_set_blob already wrote the entries, but commit forces the new page
    // state to flash so a reboot immediately after cannot roll back to the
    // previous queue. The cost is one extra page write per push/pop -- a
    // few ms, dwarfed by the TLS round-trip that accompanies every message.
    nvs_commit(s_handle);
}

void msg_store_clear(size_t capacity)
{
    nvs_erase_key(s_handle, KEY_META);
    for (size_t i = 0; i < capacity; i++) {
        char key[8];
        slot_key(i, key, sizeof(key));
        // Erasing a missing key is not an error here -- the whole point is
        // to guarantee no stale blobs survive the reset.
        nvs_erase_key(s_handle, key);
    }
    nvs_commit(s_handle);
    ESP_LOGI(TAG, "wiped persisted queue (capacity %u)", (unsigned)capacity);
}
