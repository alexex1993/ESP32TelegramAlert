#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Returned when abort_fn asked for the request to be cut short. Not a
// failure: the caller asked for it, and should not back off as if the
// network were broken.
#define HTTPS_ERR_ABORTED ((esp_err_t)0x10A01)

// Polled while the request is waiting on the socket. Returning true tears the
// connection down early -- used so a pending "read" receipt does not have to
// sit behind a 25-second long poll.
typedef bool (*https_abort_fn)(void *ctx);

typedef struct {
    const char *host;
    uint16_t port;
    const char *path;
    const char *json_body;
    int timeout_ms;          // Deadline for the whole request.
    https_abort_fn abort_fn; // Optional.
    void *abort_ctx;
} https_request_t;

// POSTs `json_body` and reads the whole response. On ESP_OK, *out_body is a
// NUL-terminated malloc'd buffer the caller must free; *out_status carries
// the HTTP status code, which may well be an error code the caller wants to
// inspect.
esp_err_t https_post_json(const https_request_t *req, int *out_status,
                           char **out_body, size_t *out_len);
