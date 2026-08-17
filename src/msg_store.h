#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
// or after a config change so an old queue cannot resurrect itself. The poll
// offset below is deliberately left alone: it is a fact about what Telegram
// has already handed over, not part of the queue's layout.
void msg_store_clear(size_t capacity);

// The getUpdates offset, in the same namespace as the queue because it
// answers the same question from the other side: the queue is what this
// device still has to show, the offset is what it has already taken delivery
// of. Telegram keeps an update until a poll acknowledges it by asking for the
// one after it, so an offset held only in RAM means every reboot re-downloads
// the batch it was in the middle of -- and pushes a second copy of each page
// that was already queued and persisted. That is what turned one crash into a
// reboot loop with a queue growing by a message a lap.
//
// Zero on first boot (or if the read fails), which is getUpdates' own "give
// me everything you are still holding".
int64_t msg_store_load_offset(void);

// Persisted only after the batch it covers is fully handled, so a crash
// mid-batch replays that batch rather than losing it. Writes nothing when the
// offset has not moved, which is every quiet long poll.
void msg_store_save_offset(int64_t offset);
