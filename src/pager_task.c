#include "pager_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_config.h"
#include "button.h"
#include "commands.h"
#include "https_client.h"
#include "msg_queue.h"
#include "net_conn.h"
#include "screen.h"
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
} ack_req_t;

static QueueHandle_t s_ack_queue;
static int64_t s_offset;

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
    if (xQueueSend(s_ack_queue, &ack, 0) != pdTRUE) {
        ESP_LOGE(TAG, "ack queue full, receipt for %lld dropped", (long long)msg.message_id);
        ui_set_status(STR_ACK_QUEUE_FULL " " LV_SYMBOL_WARNING);
        return;
    }
    ui_set_status(STR_ACK_SENDING);
}

static void drain_acks(void)
{
    ack_req_t ack;
    while (xQueueReceive(s_ack_queue, &ack, 0) == pdTRUE) {
        esp_err_t err = ESP_FAIL;
        for (int attempt = 1; attempt <= APP_ACK_MAX_ATTEMPTS; attempt++) {
            err = telegram_reply(ack.chat_id, ack.message_id, STR_RECEIPT_READ);
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "receipt attempt %d/%d failed: %s", attempt,
                     APP_ACK_MAX_ATTEMPTS, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (err == ESP_OK) {
            ui_set_status(STR_ACK_SENT " " LV_SYMBOL_OK);
        } else {
            // The message is already off the queue, so the receipt is simply
            // lost -- say so rather than pretending it went out.
            ui_set_status(STR_ACK_FAILED " " LV_SYMBOL_WARNING);
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

static void handle_new_messages(const pager_msg_t *batch, int count)
{
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "message %lld from %s", (long long)batch[i].message_id, batch[i].from);
        msg_queue_push(&batch[i]);
    }
    ui_render_queue();
    // No screen_activity() here: an arriving message must not light the glass
    // -- the RGB LED carries the unread signal now. The repaint still runs so
    // the current head is on screen the moment a key press does wake it.

#if APP_SEND_DELIVERY_RECEIPT
    for (int i = 0; i < count; i++) {
        esp_err_t err = telegram_reply(batch[i].chat_id, batch[i].message_id, STR_RECEIPT_DELIVERED);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "delivery receipt failed: %s", esp_err_to_name(err));
        }
    }
#endif

    char status[64];
    snprintf(status, sizeof(status), STR_NEW_MESSAGES_FMT "  " LV_SYMBOL_BELL, count);
    ui_set_status(status);
}

static void pager_task(void *arg)
{
    ui_render_queue();
    idle_status();
    // Boot is over and the screen has been on since display_init(); send it to
    // sleep on a countdown regardless of any restored pages, and let the LED
    // carry the unread signal until a key is pressed.
    screen_arm_off();

    // Static, not a local: this is ~5 kB at APP_MSG_TEXT_MAX, and the same
    // stack has to hold the TLS handshake and chain verification below.
    // Only this task touches it, and there is only one of it.
    static pager_msg_t batch[APP_UPDATES_PER_POLL];

    while (1) {
        drain_acks();

        int count = 0;
        esp_err_t err = telegram_poll(&s_offset, poll_should_abort, NULL,
                                       batch, APP_UPDATES_PER_POLL, &count);

        if (err == HTTPS_ERR_ABORTED) {
            // Expected: a receipt is queued and gets sent on the next lap.
            continue;
        }
        if (err != ESP_OK) {
            ui_set_status(STR_TELEGRAM_OFFLINE " " LV_SYMBOL_WARNING);
            vTaskDelay(pdMS_TO_TICKS(APP_POLL_ERROR_BACKOFF_MS));
            continue;
        }

        filtered_batch_t filtered = filter_commands(batch, count);
        if (filtered.result != COMMAND_NONE) {
            // No screen_activity() here: a command is asked remotely, and
            // lighting a pager that is lying in a drawer would answer the
            // wrong person. The footer is repainted either way -- whoever
            // picks the device up next sees what it last did. Icons are
            // limited to the six in tools/gen_fonts.sh; anything else is a box.
            ui_set_status(filtered.result == COMMAND_ANSWERED
                              ? STR_CMD_ANSWERED " " LV_SYMBOL_OK
                              : STR_CMD_FAILED " " LV_SYMBOL_WARNING);
        }

        if (filtered.kept > 0) {
            handle_new_messages(batch, filtered.kept);
        } else if (filtered.result == COMMAND_NONE && msg_queue_count() == 0) {
            // Skipped after a command: the poll returns the instant one
            // arrives, so the idle text would wipe the line above before
            // anyone could read it. The next quiet poll restores it.
            idle_status();
        }
    }
}

void pager_start(void)
{
    s_ack_queue = xQueueCreate(APP_MSG_QUEUE_LEN, sizeof(ack_req_t));
    configASSERT(s_ack_queue);

    screen_init();
    button_start(on_button_press);
    xTaskCreate(pager_task, "pager", PAGER_TASK_STACK, NULL, PAGER_TASK_PRIO, NULL);
}
