#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

// Everyone who has ever written to this pager, kept in NVS so the list
// survives a power cut, and told "the pager is back" one message at a time
// when it boots.
//
// The key is the *chat*, not the person, exactly as the receipts are: what
// the firmware can address is a chat id, so a group that pages the device is
// one entry and gets the announcement in the group. A page that arrived
// through inline mode has no chat id at all (see the inline-mode notes in
// CLAUDE.md) and is therefore not a contact -- there is nowhere to announce
// into. `name` is only ever shown in the log; the chat id is what is used.
typedef struct {
    int64_t chat_id;
    int64_t last_seen;             // Unix seconds, as the message carried it.
    char name[APP_MSG_FROM_MAX];   // Sender's display name, UTF-8.
} contact_t;

// Opens the NVS namespace and restores the list. Call once at boot, after
// nvs_flash_init(). Best-effort: if the namespace cannot be opened the list
// still works for this session, it just is not persisted.
void contacts_init(void);

// Records that `chat_id` wrote at `when`, adding it if it is new. Ignores a
// zero chat id. When the list is full the least recently seen entry is
// replaced -- a pager remembers who is still paging it.
//
// Called from the pager task only, like everything else here, which is why
// this module has no lock of its own.
void contacts_note(int64_t chat_id, const char *name, int64_t when);

size_t contacts_count(void);

// Copies the `index`-th contact in storage order. False once index reaches
// contacts_count().
bool contacts_get(size_t index, contact_t *out);

// Sends `text` to every contact, one at a time, and returns how many got it.
// Each send is a full TLS handshake, so this takes a second or two per
// contact; run it from the pager task, which has the stack for it.
size_t contacts_announce(const char *text);
