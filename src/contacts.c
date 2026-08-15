#include "contacts.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "telegram.h"

static const char *TAG = "contacts";

// Its own namespace, for the same reason settings and the message queue have
// theirs: a format change to one must not take the others with it.
static const char *NVS_NAMESPACE = "contacts";
static const char *KEY_LIST = "list";

// Bumped whenever the layout below changes. The size check in load() would
// catch a change in contact_t on its own; the version is what catches a
// reinterpretation of the same bytes.
#define CONTACTS_VERSION 1
// "PAGC" in little-endian -- a guard against reading random flash as a list.
#define CONTACTS_MAGIC   0x50414743u

// The whole list is one blob rather than a key per entry: it is ~1 kB, it is
// rewritten only when the membership actually changes (see contacts_note),
// and one write means the stored list is never half of two different ones.
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    contact_t entries[APP_CONTACTS_MAX];
} contacts_blob_t;

static nvs_handle_t s_handle;
static contacts_blob_t s_blob;

static void save(void)
{
    if (!s_handle) {
        return;   // NVS unavailable; the list still works for this session.
    }
    esp_err_t err = nvs_set_blob(s_handle, KEY_LIST, &s_blob, sizeof(s_blob));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_commit(s_handle);
}

void contacts_init(void)
{
    memset(&s_blob, 0, sizeof(s_blob));
    s_blob.magic = CONTACTS_MAGIC;
    s_blob.version = CONTACTS_VERSION;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        // Not fatal, unlike the message store: losing the contact list costs
        // an announcement, not a page someone is waiting to read.
        ESP_LOGW(TAG, "nvs_open(\"%s\") failed: %s -- contacts will not persist",
                 NVS_NAMESPACE, esp_err_to_name(err));
        s_handle = 0;
        return;
    }

    contacts_blob_t stored;
    size_t len = sizeof(stored);
    err = nvs_get_blob(s_handle, KEY_LIST, &stored, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no contacts stored yet");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s -- starting empty", esp_err_to_name(err));
        return;
    }
    if (len != sizeof(stored) || stored.magic != CONTACTS_MAGIC ||
        stored.version != CONTACTS_VERSION || stored.count > APP_CONTACTS_MAX) {
        // Another firmware's bytes. Dropped rather than decoded: the next
        // message from each chat puts its sender back on the list anyway.
        ESP_LOGW(TAG, "stored list is from another layout -- starting empty");
        return;
    }

    s_blob = stored;
    ESP_LOGI(TAG, "restored %u contact(s)", (unsigned)s_blob.count);
}

// Index of the entry that has gone longest without writing -- the one to
// replace when a new chat arrives at a full list.
static size_t least_recent(void)
{
    size_t oldest = 0;
    for (size_t i = 1; i < s_blob.count; i++) {
        if (s_blob.entries[i].last_seen < s_blob.entries[oldest].last_seen) {
            oldest = i;
        }
    }
    return oldest;
}

void contacts_note(int64_t chat_id, const char *name, int64_t when)
{
    if (chat_id == 0) {
        return;   // An inline page: no chat to announce into.
    }
    if (!name) {
        name = "?";
    }

    for (size_t i = 0; i < s_blob.count; i++) {
        contact_t *entry = &s_blob.entries[i];
        if (entry->chat_id != chat_id) {
            continue;
        }
        // `last_seen` is only ever the value that is on flash, so the two
        // cannot disagree -- and refreshing it costs a write, so it is
        // refreshed at most once every APP_CONTACTS_TOUCH_S. It orders the
        // eviction below and nothing else, and for that a stamp that lags by
        // an hour is exactly as good as an exact one.
        bool changed = false;
        if (strcmp(entry->name, name) != 0) {
            snprintf(entry->name, sizeof(entry->name), "%s", name);
            changed = true;
        }
        if (when > entry->last_seen + APP_CONTACTS_TOUCH_S) {
            entry->last_seen = when;
            changed = true;
        }
        if (changed) {
            save();
        }
        return;
    }

    size_t slot;
    if (s_blob.count < APP_CONTACTS_MAX) {
        slot = s_blob.count++;
    } else {
        slot = least_recent();
        ESP_LOGW(TAG, "contact list full, forgetting chat %lld (%s)",
                 (long long)s_blob.entries[slot].chat_id, s_blob.entries[slot].name);
    }

    contact_t *entry = &s_blob.entries[slot];
    entry->chat_id = chat_id;
    entry->last_seen = when;
    // Plain snprintf, not a UTF-8-aware copy: `name` comes from pager_msg_t.from,
    // which telegram.c already cut on a character boundary to this same size.
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    save();

    ESP_LOGI(TAG, "new contact: chat %lld (%s), %u total",
             (long long)chat_id, entry->name, (unsigned)s_blob.count);
}

size_t contacts_count(void)
{
    return s_blob.count;
}

bool contacts_get(size_t index, contact_t *out)
{
    if (index >= s_blob.count) {
        return false;
    }
    *out = s_blob.entries[index];
    return true;
}

size_t contacts_announce(const char *text)
{
    size_t sent = 0;
    for (size_t i = 0; i < s_blob.count; i++) {
        int64_t chat_id = s_blob.entries[i].chat_id;
        esp_err_t err = telegram_send_message(chat_id, text);
        if (err == ESP_OK) {
            sent++;
            continue;
        }
        // Kept on the list rather than dropped. A failure here is far more
        // often a link that is not up yet than a chat that has blocked the
        // bot, and forgetting someone costs them every future announcement
        // while retrying costs one handshake per boot.
        ESP_LOGW(TAG, "announcement to chat %lld failed: %s",
                 (long long)chat_id, esp_err_to_name(err));
    }
    return sent;
}
