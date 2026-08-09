#pragma once

// Starts the button watcher and the task that long-polls Telegram, queues
// incoming messages and sends read receipts. Call once, after WiFi is up and
// https_client_init() has run.
void pager_start(void);
