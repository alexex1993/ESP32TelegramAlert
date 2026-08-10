#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "msg_queue.h"

// NVS-backed persistence for the message queue. Every push and pop writes
// through to flash so an unread page is not lost to a power cut or a crash.
// The store lives in its own translation unit so the in-memory ring buffer
// in msg_queue.c stays focused on its mutex and indices; this file owns all
// nvs_open / nvs_set_blob / nvs_get_blob calls.

// Opens the "pager" NVS namespace. Idempotent; safe to call from msg_queue
// before any load/save. Hard-fails (configASSERT) if NVS cannot be opened,
// because losing the handle makes every later save a silent no-op -- a
// pager that pretends to persist is worse than one that says it cannot.
void msg_store_init(void);

// Restores whatever was persisted. On success fills slots[] (only the live
// indices [head, head+count) are touched), sets *out_head / *out_count /
// *out_dropped, and returns true. On first boot, version mismatch, size
// mismatch or corruption returns false and leaves the outputs untouched --
// the caller is expected to msg_store_clear() in that case so stale slot
// blobs from a previous firmware cannot leak back in.
bool msg_store_load(pager_msg_t *slots, size_t capacity,
                    size_t *out_head, size_t *out_count, size_t *out_dropped);

// Persists a single ring slot at index idx. Called after a push writes into
// slots[idx], and before msg_store_save_meta() -- see the order note above.
void msg_store_save_slot(size_t idx, const pager_msg_t *msg);

// Persists the queue bookkeeping. Called after every push and pop.
void msg_store_save_meta(size_t head, size_t count, size_t dropped);

// Erases meta and every slot key in [0, capacity). Used on a load failure
// or after a config change so an old queue cannot resurrect itself.
void msg_store_clear(size_t capacity);
