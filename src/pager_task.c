#include "pager_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_config.h"
#include "button.h"
#include "commands.h"
#include "contacts.h"
#include "https_client.h"
#include "inline_mode.h"
#include "msg_queue.h"
#include "msg_store.h"
#include "net_conn.h"
#include "sd_log.h"
#include "screen.h"
#include "settings.h"
#include "telegram.h"
#include "ui.h"
#include "ui_strings.h"

static const char *TAG = "pager";

// Room for the TLS handshake and certificate-chain verification, which are by
// far the deepest thing this task does.
#define PAGER_TASK_STACK 10240
#define PAGER_TASK_PRIO  4

typedef struct {
    int64_t chat_id;
    int64_t message_id;
    // Set for a page that arrived inline, which has no chat to react in. Its
    // receipt edits the message the inline card posted, and editMessageText
    // replaces the whole body -- so the text has to travel with the request.
    // On the heap rather than in a field: a field would put
    // APP_INLINE_QUERY_MAX bytes in every one of this queue's slots for the
    // sake of the few that are inline. drain_acks() owns the free.
    char inline_message_id[APP_INLINE_MSG_ID_MAX];
    char *inline_text;
} ack_req_t;

static QueueHandle_t s_ack_queue;
static int64_t s_offset;
// What of s_offset is on flash. The two only differ between a poll that
// brought something back and the end of the lap that handled it.
static int64_t s_saved_offset;

// Cuts a long poll short when a receipt is waiting. Without this the user
// would press the button and then watch nothing happen until the poll's
// 25-second window expired.
static bool poll_should_abort(void *ctx)
{
    return uxQueueMessagesWaiting(s_ack_queue) > 0;
}

static void idle_status(void)
{
    ui_set_status(net_conn_uses_proxy() ? STR_WAITING_PROXY : STR_WAITING);
}

static void on_button_press(void)
{
    // The key that acknowledges is also the key that wakes, so a press on a
    // dark screen only lights it. Nothing is queued in that state anyway (an
    // unread message keeps the backlight on), but the guard keeps the wake
    // press from flashing "nothing to acknowledge" at someone who has just
    // picked the pager up.
    if (!screen_is_on()) {
        screen_activity();
        return;
    }

    pager_msg_t msg;
    if (!msg_queue_pop(&msg)) {
        ui_set_status(STR_NOTHING_TO_ACK);
        screen_activity();
        return;
    }

    ESP_LOGI(TAG, "acknowledging message %lld", (long long)msg.message_id);
    ui_render_queue();
    // Popped the last one? This starts the countdown to sleep.
    screen_activity();

    ack_req_t ack = { .chat_id = msg.chat_id, .message_id = msg.message_id };
    if (pager_msg_is_inline(&msg)) {
        snprintf(ack.inline_message_id, sizeof(ack.inline_message_id), "%s",
                 msg.inline_message_id);
        // A failed strdup leaves inline_text NULL, which drain_acks reports as
        // an undelivered receipt -- the same outcome as a refused edit, and
        // the page is off the queue either way.
        ack.inline_text = strdup(msg.text);
        if (!ack.inline_text) {
            ESP_LOGE(TAG, "no memory for the inline receipt text");
        }
    }

    if (xQueueSend(s_ack_queue, &ack, 0) != pdTRUE) {
        ESP_LOGE(TAG, "ack queue full, receipt for %lld dropped", (long long)msg.message_id);
        ui_set_statusf("%s %s", STR_ACK_QUEUE_FULL, LV_SYMBOL_WARNING);
        free(ack.inline_text);
        return;
    }
    ui_set_status(STR_ACK_SENDING);
}

// Marks a message in the chat it came from. A reaction rather than a reply:
// one emoji on the message itself instead of a second message under it, and
// the bot's single reaction means "read" replaces "delivered" rather than
// piling up next to it. Channels refuse bot reactions and any chat can turn
// them off, so the old text reply is kept as the fallback -- a refused
// reaction must not cost the receipt.
static bool mark_chat_message(int64_t chat_id, int64_t message_id,
                              const char *emoji, const char *fallback)
{
    if (telegram_set_reaction(chat_id, message_id, emoji) == ESP_OK) {
        return true;
    }
    ESP_LOGI(TAG, "reaction refused in chat %lld, replying instead", (long long)chat_id);
    return telegram_reply(chat_id, message_id, fallback) == ESP_OK;
}

// The same mark for a page that arrived inline, where there is no chat id to
// react in -- the message the inline card posted is rewritten with the emoji
// appended, which also drops the pending button.
static bool mark_inline_page(const char *inline_message_id, const char *text,
                             const char *emoji)
{
    // Static for the same reason the poll batch is: only the pager task gets
    // here, and its stack has to hold the TLS handshake this feeds. Sized for
    // an inline page, whose text is a query and so far below APP_MSG_TEXT_MAX.
    static char s_edited[APP_INLINE_QUERY_MAX + 8];
    snprintf(s_edited, sizeof(s_edited), "%s %s", text, emoji);
    return telegram_edit_inline_text(inline_message_id, s_edited) == ESP_OK;
}

static bool send_read_receipt(const ack_req_t *ack)
{
    if (ack->inline_message_id[0] != '\0') {
        if (!ack->inline_text) {
            return false;
        }
        return mark_inline_page(ack->inline_message_id, ack->inline_text, STR_REACTION_READ);
    }
    if (ack->chat_id == 0) {
        // Neither a chat nor an inline handle: the page was shown but can
        // never be answered. See parse_chosen_inline() in telegram.c.
        ESP_LOGW(TAG, "page %lld has nowhere to report to", (long long)ack->message_id);
        return false;
    }
    return mark_chat_message(ack->chat_id, ack->message_id, STR_REACTION_READ,
                             STR_RECEIPT_READ);
}

static void drain_acks(void)
{
    ack_req_t ack;
    while (xQueueReceive(s_ack_queue, &ack, 0) == pdTRUE) {
        bool sent = false;
        for (int attempt = 1; attempt <= APP_ACK_MAX_ATTEMPTS; attempt++) {
            sent = send_read_receipt(&ack);
            if (sent) {
                break;
            }
            ESP_LOGW(TAG, "receipt attempt %d/%d failed", attempt, APP_ACK_MAX_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        free(ack.inline_text);

        if (sent) {
            ui_set_statusf("%s %s", STR_ACK_SENT, LV_SYMBOL_OK);
        } else {
            // The message is already off the queue, so the receipt is simply
            // lost -- say so rather than pretending it went out.
            ui_set_statusf("%s %s", STR_ACK_FAILED, LV_SYMBOL_WARNING);
        }
    }
}

typedef struct {
    int kept;                 // Messages left in the batch, to be paged.
    command_result_t result;  // COMMAND_NONE when the batch held no commands.
} filtered_batch_t;

// Answers the commands in a batch and closes the gaps they leave, so nothing
// downstream ever sees them: a /status must not take a queue slot, wake the
// screen or draw a "delivered" receipt on top of its own answer.
static filtered_batch_t filter_commands(pager_msg_t *batch, int count)
{
    filtered_batch_t out = { .kept = 0, .result = COMMAND_NONE };

    for (int i = 0; i < count; i++) {
        command_result_t result = commands_try_handle(&batch[i]);
        if (result != COMMAND_NONE) {
            // When a batch holds both, the failure is the one worth reporting.
            if (out.result != COMMAND_FAILED) {
                out.result = result;
            }
            continue;
        }
        if (out.kept != i) {
            batch[out.kept] = batch[i];
        }
        out.kept++;
    }

    return out;
}

static void send_delivery_receipt(const pager_msg_t *msg)
{
    if (pager_msg_is_inline(msg)) {
        // Not under APP_SEND_DELIVERY_RECEIPT, unlike the chat path below:
        // this edit is what clears the pending button the inline card carried,
        // and a button left spinning forever promises a report that a compile-
        // time flag has quietly cancelled.
        if (!mark_inline_page(msg->inline_message_id, msg->text, STR_REACTION_DELIVERED)) {
            ESP_LOGW(TAG, "could not mark inline page as delivered");
        }
        return;
    }
    if (msg->chat_id == 0) {
        return;
    }
#if APP_SEND_DELIVERY_RECEIPT
    if (!mark_chat_message(msg->chat_id, msg->message_id, STR_REACTION_DELIVERED,
                           STR_RECEIPT_DELIVERED)) {
        ESP_LOGW(TAG, "delivery receipt failed for message %lld", (long long)msg->message_id);
    }
#endif
}

static void handle_new_messages(const pager_msg_t *batch, int count)
{
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "message %lld from %s", (long long)batch[i].message_id, batch[i].from);
        // Log to the SD card before pushing: if the push evicts an older page
        // on a full queue, the new arrival is at least on the card, and a write
        // hiccup here never blocks the push below it.
        sd_log_message(&batch[i]);
        msg_queue_push(&batch[i]);
    }
    ui_render_queue();
    // No screen_activity() here: an arriving message must not light the glass
    // -- the RGB LED carries the unread signal now. The repaint still runs so
    // the current head is on screen the moment a key press does wake it.

    for (int i = 0; i < count; i++) {
        send_delivery_receipt(&batch[i]);
    }

    char tmp[40];
    snprintf(tmp, sizeof(tmp), STR_NEW_MESSAGES_FMT, count);
    ui_set_statusf("%s  %s", tmp, LV_SYMBOL_BELL);
}

// Telegram fires a fresh inline_query on every keystroke, and answering one
// costs this device a full TLS handshake -- so a batch can easily hold three
// versions of a sentence someone is still typing. Only the last one per user
// can still be shown (Telegram discards the answer to a superseded query), so
// the earlier ones are dropped rather than paid for.
static int dedupe_queries(telegram_inline_query_t *queries, int count)
{
    int kept = 0;
    for (int i = 0; i < count; i++) {
        bool superseded = false;
        for (int j = i + 1; j < count; j++) {
            if (queries[j].from_id == queries[i].from_id) {
                superseded = true;
                break;
            }
        }
        if (superseded) {
            continue;
        }
        if (kept != i) {
            queries[kept] = queries[i];
        }
        kept++;
    }
    return kept;
}

// Answers everything in the batch that asks the device about itself rather
// than paging it: inline queries and taps on a pending button. Returns the
// same verdict filter_commands() does -- the three of them are one category,
// remote requests the firmware answers on the spot.
static command_result_t answer_inline(telegram_batch_t *batch)
{
    command_result_t result = COMMAND_NONE;

    batch->queries_count = dedupe_queries(batch->queries, batch->queries_count);
    for (int i = 0; i < batch->queries_count; i++) {
        bool ok = inline_answer_query(&batch->queries[i]);
        if (!ok || result == COMMAND_NONE) {
            result = ok ? COMMAND_ANSWERED : COMMAND_FAILED;
        }
    }

    for (int i = 0; i < batch->callbacks_count; i++) {
        // The one button the firmware draws, tapped while the page is still in
        // flight. Answering it dismisses the spinner; the toast says why the
        // hourglass is still there.
        bool ok = telegram_answer_callback(batch->callbacks[i].id,
                                            STR_INLINE_PENDING_ALERT) == ESP_OK;
        if (!ok || result == COMMAND_NONE) {
            result = ok ? COMMAND_ANSWERED : COMMAND_FAILED;
        }
    }

    return result;
}

static void pager_task(void *arg)
{
    ui_render_queue();
    idle_status();
    // Boot is over and the screen has been on since display_init(); send it to
    // sleep on a countdown regardless of any restored pages, and let the LED
    // carry the unread signal until a key is pressed.
    screen_arm_off();

#if APP_ANNOUNCE_ON_BOOT
    // Tell everyone who has ever paged this device that it is back. Before the
    // first poll rather than after one, because that is what makes the message
    // mean "I have just come up" -- and it costs a handshake per contact, so
    // the poll it delays would have been delayed by the same seconds anyway.
    // Failures are logged inside contacts_announce() and cost nothing else.
    if (contacts_count() > 0) {
        ui_set_status(STR_BOOT_ANNOUNCING);
        size_t sent = contacts_announce(STR_BOOT_ANNOUNCE);
        ESP_LOGI(TAG, "boot announcement reached %u of %u contact(s)",
                 (unsigned)sent, (unsigned)contacts_count());
        idle_status();
    }
#endif

    // Static, not locals: the messages alone are ~10 kB at APP_MSG_TEXT_MAX,
    // and the same stack has to hold the TLS handshake and chain verification
    // below. Only this task touches them, and there is only one of it.
    static pager_msg_t msgs[APP_UPDATES_PER_POLL];
    static telegram_inline_query_t queries[APP_INLINE_QUERIES_PER_POLL];
    static telegram_callback_t callbacks[APP_INLINE_CALLBACKS_PER_POLL];
    telegram_batch_t batch = {
        .msgs = msgs,           .msgs_max = APP_UPDATES_PER_POLL,
        .queries = queries,     .queries_max = APP_INLINE_QUERIES_PER_POLL,
        .callbacks = callbacks, .callbacks_max = APP_INLINE_CALLBACKS_PER_POLL,
    };

    while (1) {
        drain_acks();

        esp_err_t err = telegram_poll(&s_offset, poll_should_abort, NULL, &batch);

        if (err == HTTPS_ERR_ABORTED) {
            // Expected: a receipt is queued and gets sent on the next lap.
            continue;
        }
        if (err != ESP_OK) {
            ui_set_statusf("%s %s", STR_TELEGRAM_OFFLINE, LV_SYMBOL_WARNING);
            vTaskDelay(pdMS_TO_TICKS(APP_POLL_ERROR_BACKOFF_MS));
            continue;
        }

        // Remembered before anything filters the batch, so a chat that only
        // ever sends commands is on the list too: /status is as much "I am
        // talking to this pager" as a page is, and its sender wants the boot
        // announcement just the same. An inline page carries no chat id and is
        // ignored by contacts_note().
        for (int i = 0; i < batch.msgs_count; i++) {
            contacts_note(batch.msgs[i].chat_id, batch.msgs[i].from, batch.msgs[i].date);
        }

        // Inline first: an inline query has a few seconds before Telegram
        // stops caring, while a page on the glass waits for a human anyway.
        command_result_t inline_result = answer_inline(&batch);
        filtered_batch_t filtered = filter_commands(batch.msgs, batch.msgs_count);

        // No screen_activity() for either: both are asked remotely, and
        // lighting a pager that is lying in a drawer would answer the wrong
        // person. The footer is repainted regardless -- whoever picks the
        // device up next sees what it last did. Icons are limited to the six
        // in tools/gen_fonts.sh; anything else is a box.
        if (filtered.result != COMMAND_NONE) {
            ui_set_statusf("%s %s",
                           filtered.result == COMMAND_ANSWERED ? STR_CMD_ANSWERED : STR_CMD_FAILED,
                           filtered.result == COMMAND_ANSWERED ? LV_SYMBOL_OK : LV_SYMBOL_WARNING);
        }
        if (inline_result != COMMAND_NONE) {
            ui_set_statusf("%s %s",
                           inline_result == COMMAND_ANSWERED ? STR_INLINE_ANSWERED : STR_INLINE_FAILED,
                           inline_result == COMMAND_ANSWERED ? LV_SYMBOL_OK : LV_SYMBOL_WARNING);
        }

        if (filtered.kept > 0) {
            handle_new_messages(batch.msgs, filtered.kept);
        } else if (filtered.result == COMMAND_NONE && inline_result == COMMAND_NONE &&
                   msg_queue_count() == 0) {
            // Skipped after a command or an inline answer: the poll returns
            // the instant one arrives, so the idle text would wipe the line
            // above before anyone could read it. The next quiet poll restores
            // it.
            idle_status();
        }

        // Last thing in the lap, on purpose. Every message in this batch is on
        // the card, in the queue and persisted there by now, so writing the
        // offset here can only ever acknowledge a page that is already safe --
        // and a crash anywhere above replays the batch instead of losing it.
        // That is why the offset is written here rather than where
        // telegram_poll() advances it.
        if (s_offset != s_saved_offset) {
            msg_store_save_offset(s_offset);
            s_saved_offset = s_offset;
        }
    }
}

void pager_start(void)
{
    s_ack_queue = xQueueCreate(APP_MSG_QUEUE_LEN, sizeof(ack_req_t));
    configASSERT(s_ack_queue);

    // Resume where the last boot left off. Without this a reboot re-downloads
    // the batch that was in flight and pushes a second copy of every page it
    // had already queued -- the queue grows by a message on every restart,
    // which is what made a single crash look like a device stuck in a loop.
    // msg_store_init() has already run, from msg_queue_init() in app_main.
    s_offset = msg_store_load_offset();
    s_saved_offset = s_offset;

    screen_init();
    // on_long: a 5 s BOOT hold sets the force-ap flag and reboots into the
    // provisioning portal, pre-filled with the current settings -- the recovery
    // gesture for "moved to a new WiFi" without a serial cable.
    button_start(on_button_press, settings_request_ap_and_restart);
    xTaskCreate(pager_task, "pager", PAGER_TASK_STACK, NULL, PAGER_TASK_PRIO, NULL);
}
