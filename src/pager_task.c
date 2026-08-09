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
#include "https_client.h"
#include "msg_queue.h"
#include "net_conn.h"
#include "telegram.h"
#include "ui.h"

static const char *TAG = "pager";

// Room for the TLS handshake and certificate-chain verification, which are by
// far the deepest thing this task does.
#define PAGER_TASK_STACK 10240
#define PAGER_TASK_PRIO  4

#define ACK_TEXT      "\xE2\x9C\x85 \xD0\x9F\xD1\x80\xD0\xBE\xD1\x87\xD0\xB8\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE" // "✅ Прочитано"
#define DELIVERED_TEXT "\xF0\x9F\x93\xA8 \xD0\x94\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD0\xB2\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xBE" // "📨 Доставлено"

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
    ui_set_status(net_conn_uses_proxy()
                      ? "Ожидание сообщений (SOCKS5)"
                      : "Ожидание сообщений");
}

static void on_button_press(void)
{
    pager_msg_t msg;
    if (!msg_queue_pop(&msg)) {
        ui_set_status("Нечего подтверждать");
        return;
    }

    ESP_LOGI(TAG, "acknowledging message %lld", (long long)msg.message_id);
    ui_render_queue();

    ack_req_t ack = { .chat_id = msg.chat_id, .message_id = msg.message_id };
    if (xQueueSend(s_ack_queue, &ack, 0) != pdTRUE) {
        ESP_LOGE(TAG, "ack queue full, receipt for %lld dropped", (long long)msg.message_id);
        ui_set_status("Очередь подтверждений переполнена " LV_SYMBOL_WARNING);
        return;
    }
    ui_set_status("Отправляю «прочитано»...");
}

static void drain_acks(void)
{
    ack_req_t ack;
    while (xQueueReceive(s_ack_queue, &ack, 0) == pdTRUE) {
        esp_err_t err = ESP_FAIL;
        for (int attempt = 1; attempt <= APP_ACK_MAX_ATTEMPTS; attempt++) {
            err = telegram_reply(ack.chat_id, ack.message_id, ACK_TEXT);
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "receipt attempt %d/%d failed: %s", attempt,
                     APP_ACK_MAX_ATTEMPTS, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (err == ESP_OK) {
            ui_set_status("Подтверждено " LV_SYMBOL_OK);
        } else {
            // The message is already off the queue, so the receipt is simply
            // lost -- say so rather than pretending it went out.
            ui_set_status("Подтверждение не доставлено " LV_SYMBOL_WARNING);
        }
    }
}

static void handle_new_messages(const pager_msg_t *batch, int count)
{
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "message %lld from %s", (long long)batch[i].message_id, batch[i].from);
        msg_queue_push(&batch[i]);
    }
    ui_render_queue();

#if APP_SEND_DELIVERY_RECEIPT
    for (int i = 0; i < count; i++) {
        esp_err_t err = telegram_reply(batch[i].chat_id, batch[i].message_id, DELIVERED_TEXT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "delivery receipt failed: %s", esp_err_to_name(err));
        }
    }
#endif

    char status[64];
    snprintf(status, sizeof(status), "Новых: %d  " LV_SYMBOL_BELL, count);
    ui_set_status(status);
}

static void pager_task(void *arg)
{
    ui_render_queue();
    idle_status();

    pager_msg_t batch[APP_UPDATES_PER_POLL];

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
            ui_set_status("Нет связи с Telegram, повтор... " LV_SYMBOL_WARNING);
            vTaskDelay(pdMS_TO_TICKS(APP_POLL_ERROR_BACKOFF_MS));
            continue;
        }

        if (count > 0) {
            handle_new_messages(batch, count);
        } else if (msg_queue_count() == 0) {
            idle_status();
        }
    }
}

void pager_start(void)
{
    s_ack_queue = xQueueCreate(APP_MSG_QUEUE_LEN, sizeof(ack_req_t));
    configASSERT(s_ack_queue);

    button_start(on_button_press);
    xTaskCreate(pager_task, "pager", PAGER_TASK_STACK, NULL, PAGER_TASK_PRIO, NULL);
}
