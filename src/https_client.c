#include "https_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include "app_config.h"
#include "net_conn.h"

static const char *TAG = "https";

#define RESPONSE_INITIAL_CAP 2048
#define READ_CHUNK           1024
// How long a single select() waits before the abort callback and the overall
// deadline get another look.
#define WAIT_SLICE_MS        200

// ---- request-scoped waiting --------------------------------------------

typedef struct {
    int fd;
    int64_t deadline_us;
    const https_request_t *req;
} wait_ctx_t;

static esp_err_t wait_for_socket(wait_ctx_t *w, bool for_write)
{
    if (w->req->abort_fn && w->req->abort_fn(w->req->abort_ctx)) {
        return HTTPS_ERR_ABORTED;
    }
    if (esp_timer_get_time() >= w->deadline_us) {
        return ESP_ERR_TIMEOUT;
    }

    fd_set set;
    FD_ZERO(&set);
    FD_SET(w->fd, &set);
    struct timeval tv = { .tv_sec = 0, .tv_usec = WAIT_SLICE_MS * 1000 };
    select(w->fd + 1, for_write ? NULL : &set, for_write ? &set : NULL, NULL, &tv);
    return ESP_OK;
}

// ---- response buffer ----------------------------------------------------

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t resp_append(resp_buf_t *r, const char *data, size_t len)
{
    size_t needed = r->len + len + 1; // +1 for NUL
    if (needed > APP_HTTP_MAX_RESPONSE) {
        ESP_LOGE(TAG, "response exceeds %d bytes", APP_HTTP_MAX_RESPONSE);
        return ESP_ERR_NO_MEM;
    }
    if (needed > r->cap) {
        size_t cap = r->cap ? r->cap : RESPONSE_INITIAL_CAP;
        while (cap < needed) {
            cap *= 2;
        }
        char *grown = realloc(r->buf, cap);
        if (!grown) {
            return ESP_ERR_NO_MEM;
        }
        r->buf = grown;
        r->cap = cap;
    }
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

// Splits headers from body in place, leaving the body at the front of the
// buffer so the caller can hand back the same allocation.
static esp_err_t split_response(resp_buf_t *r, int *out_status)
{
    if (r->len == 0) {
        ESP_LOGE(TAG, "empty response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    int major = 0, minor = 0, status = 0;
    if (sscanf(r->buf, "HTTP/%d.%d %d", &major, &minor, &status) != 3) {
        ESP_LOGE(TAG, "malformed status line");
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_status = status;

    const char *sep = strstr(r->buf, "\r\n\r\n");
    if (!sep) {
        ESP_LOGE(TAG, "no header terminator in response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t body_off = (size_t)(sep - r->buf) + 4;
    size_t body_len = r->len - body_off;
    memmove(r->buf, r->buf + body_off, body_len);
    r->len = body_len;
    r->buf[body_len] = '\0';
    return ESP_OK;
}

// ---- request ------------------------------------------------------------

static void log_mbedtls_error(const char *what, int ret)
{
    char err_buf[128];
    mbedtls_strerror(ret, err_buf, sizeof(err_buf));
    ESP_LOGE(TAG, "%s failed: -0x%04x (%s)", what, -ret, err_buf);
}

static esp_err_t ssl_write_all(mbedtls_ssl_context *ssl, wait_ctx_t *w,
                                const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int ret = mbedtls_ssl_write(ssl, (const unsigned char *)data + sent, len - sent);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            esp_err_t err = wait_for_socket(w, ret == MBEDTLS_ERR_SSL_WANT_WRITE);
            if (err != ESP_OK) {
                return err;
            }
            continue;
        }
        if (ret <= 0) {
            log_mbedtls_error("mbedtls_ssl_write", ret);
            return ESP_FAIL;
        }
        sent += (size_t)ret;
    }
    return ESP_OK;
}

esp_err_t https_post_json(const https_request_t *req, int *out_status,
                           char **out_body, size_t *out_len)
{
    int fd = -1;
    esp_err_t err = net_conn_open(req->host, req->port, APP_HTTP_CONNECT_TIMEOUT_MS,
                                   req->timeout_ms, &fd);
    if (err != ESP_OK) {
        return err;
    }
    // mbedTLS only reports WANT_READ/WANT_WRITE on a non-blocking socket; on a
    // blocking one a timed-out read looks like a hard failure instead, and the
    // abort callback would never get a chance to run.
    if (net_conn_set_nonblocking(fd) != ESP_OK) {
        net_conn_close(fd);
        return ESP_FAIL;
    }

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_net_context server_fd;
    resp_buf_t resp = {0};

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_net_init(&server_fd);
    server_fd.fd = fd;

    wait_ctx_t wait = {
        .fd = fd,
        .deadline_us = esp_timer_get_time() + (int64_t)req->timeout_ms * 1000,
        .req = req,
    };

    int ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        log_mbedtls_error("mbedtls_ssl_config_defaults", ret);
        err = ESP_FAIL;
        goto cleanup;
    }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    // The bundled root store, rather than a pinned certificate: Telegram
    // rotates issuers, and a proxy does not change who we must authenticate.
    ret = esp_crt_bundle_attach(&conf);
    if (ret != 0) {
        log_mbedtls_error("esp_crt_bundle_attach", ret);
        err = ESP_FAIL;
        goto cleanup;
    }
    // No RNG to wire up: mbedTLS 4 draws randomness from PSA crypto, which it
    // initialises itself on first use.

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) {
        log_mbedtls_error("mbedtls_ssl_setup", ret);
        err = ESP_FAIL;
        goto cleanup;
    }
    // Both SNI and the name checked against the certificate.
    ret = mbedtls_ssl_set_hostname(&ssl, req->host);
    if (ret != 0) {
        log_mbedtls_error("mbedtls_ssl_set_hostname", ret);
        err = ESP_FAIL;
        goto cleanup;
    }
    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            err = wait_for_socket(&wait, ret == MBEDTLS_ERR_SSL_WANT_WRITE);
            if (err != ESP_OK) {
                goto cleanup;
            }
            continue;
        }
        log_mbedtls_error("TLS handshake", ret);
        err = ESP_FAIL;
        goto cleanup;
    }

    uint32_t verify = mbedtls_ssl_get_verify_result(&ssl);
    if (verify != 0) {
        ESP_LOGE(TAG, "certificate verification failed: 0x%08x", (unsigned)verify);
        err = ESP_FAIL;
        goto cleanup;
    }

    size_t body_len = req->json_body ? strlen(req->json_body) : 0;
    char header[512];
    // HTTP/1.0 on purpose. It rules out chunked transfer-encoding, so "read
    // until the peer closes" is an exact read of the whole body and this
    // client needs no chunk parser.
    int header_len = snprintf(header, sizeof(header),
                              "POST %s HTTP/1.0\r\n"
                              "Host: %s\r\n"
                              "User-Agent: esp32-telegram-pager\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: %u\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              req->path, req->host, (unsigned)body_len);
    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        ESP_LOGE(TAG, "request headers do not fit");
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    err = ssl_write_all(&ssl, &wait, header, (size_t)header_len);
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (body_len > 0) {
        err = ssl_write_all(&ssl, &wait, req->json_body, body_len);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    char chunk[READ_CHUNK];
    bool done = false;
    while (!done) {
        ret = mbedtls_ssl_read(&ssl, (unsigned char *)chunk, sizeof(chunk));
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            err = wait_for_socket(&wait, ret == MBEDTLS_ERR_SSL_WANT_WRITE);
            if (err != ESP_OK) {
                goto cleanup;
            }
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) {
            done = true;
            break;
        }
        if (ret < 0) {
            // Some peers drop the connection without a close_notify. If the
            // response already arrived that is merely untidy, not a failure.
            if ((ret == MBEDTLS_ERR_NET_CONN_RESET || ret == MBEDTLS_ERR_SSL_CONN_EOF)
                && resp.len > 0) {
                done = true;
                break;
            }
            log_mbedtls_error("mbedtls_ssl_read", ret);
            err = ESP_FAIL;
            goto cleanup;
        }
        err = resp_append(&resp, chunk, (size_t)ret);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    err = split_response(&resp, out_status);
    if (err != ESP_OK) {
        goto cleanup;
    }

    *out_body = resp.buf;
    *out_len = resp.len;
    resp.buf = NULL; // ownership handed to the caller

cleanup:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    net_conn_close(fd);
    free(resp.buf);
    return err;
}
