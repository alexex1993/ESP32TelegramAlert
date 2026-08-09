#include "socks5.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "socks5";

#define SOCKS5_VERSION       0x05
#define SOCKS5_AUTH_NONE     0x00
#define SOCKS5_AUTH_USERPASS 0x02
#define SOCKS5_AUTH_NONE_ACCEPTABLE 0xFF

#define SOCKS5_CMD_CONNECT   0x01
#define SOCKS5_ATYP_IPV4     0x01
#define SOCKS5_ATYP_DOMAIN   0x03
#define SOCKS5_ATYP_IPV6     0x04

#define SOCKS5_USERPASS_VERSION 0x01

// Greeting (4) + auth (2 + 255 + 255) + connect request (7 + 255): the
// largest single message is the auth one, so one buffer sized for it is
// enough for every step.
#define SOCKS5_MAX_MSG 520

static const char *reply_error(uint8_t rep)
{
    switch (rep) {
    case 0x01: return "general SOCKS server failure";
    case 0x02: return "connection not allowed by ruleset";
    case 0x03: return "network unreachable";
    case 0x04: return "host unreachable";
    case 0x05: return "connection refused";
    case 0x06: return "TTL expired";
    case 0x07: return "command not supported";
    case 0x08: return "address type not supported";
    default:   return "unknown error";
    }
}

static esp_err_t write_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "send failed: errno %d", errno);
            return ESP_FAIL;
        }
        sent += (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t read_all(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = recv(fd, buf + got, len - got, 0);
        if (n == 0) {
            ESP_LOGE(TAG, "proxy closed the connection mid-handshake");
            return ESP_FAIL;
        }
        if (n < 0) {
            ESP_LOGE(TAG, "recv failed: errno %d", errno);
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t negotiate_method(int fd, bool want_auth, uint8_t *out_method)
{
    uint8_t greeting[4];
    size_t len = 0;
    greeting[len++] = SOCKS5_VERSION;
    if (want_auth) {
        greeting[len++] = 2;
        greeting[len++] = SOCKS5_AUTH_NONE;
        greeting[len++] = SOCKS5_AUTH_USERPASS;
    } else {
        greeting[len++] = 1;
        greeting[len++] = SOCKS5_AUTH_NONE;
    }
    if (write_all(fd, greeting, len) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t reply[2];
    if (read_all(fd, reply, sizeof(reply)) != ESP_OK) {
        return ESP_FAIL;
    }
    if (reply[0] != SOCKS5_VERSION) {
        ESP_LOGE(TAG, "proxy answered with version 0x%02x, expected SOCKS5", reply[0]);
        return ESP_FAIL;
    }
    if (reply[1] == SOCKS5_AUTH_NONE_ACCEPTABLE) {
        ESP_LOGE(TAG, "proxy rejected every offered auth method%s",
                 want_auth ? "" : " (no credentials configured -- set PROXY_USER/PROXY_PASS)");
        return ESP_FAIL;
    }
    if (reply[1] != SOCKS5_AUTH_NONE && reply[1] != SOCKS5_AUTH_USERPASS) {
        ESP_LOGE(TAG, "proxy chose unsupported auth method 0x%02x", reply[1]);
        return ESP_FAIL;
    }

    *out_method = reply[1];
    return ESP_OK;
}

static esp_err_t authenticate(int fd, const char *user, const char *pass)
{
    size_t user_len = strlen(user);
    size_t pass_len = strlen(pass);
    if (user_len == 0 || user_len > 255 || pass_len == 0 || pass_len > 255) {
        ESP_LOGE(TAG, "proxy asked for username/password auth but credentials are unusable");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t msg[SOCKS5_MAX_MSG];
    size_t len = 0;
    msg[len++] = SOCKS5_USERPASS_VERSION;
    msg[len++] = (uint8_t)user_len;
    memcpy(msg + len, user, user_len);
    len += user_len;
    msg[len++] = (uint8_t)pass_len;
    memcpy(msg + len, pass, pass_len);
    len += pass_len;

    if (write_all(fd, msg, len) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t reply[2];
    if (read_all(fd, reply, sizeof(reply)) != ESP_OK) {
        return ESP_FAIL;
    }
    // RFC 1929: any non-zero status means failure and the server closes.
    if (reply[1] != 0x00) {
        ESP_LOGE(TAG, "proxy rejected the credentials (status 0x%02x)", reply[1]);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t request_connect(int fd, const char *dest_host, uint16_t dest_port)
{
    size_t host_len = strlen(dest_host);
    if (host_len == 0 || host_len > 255) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t msg[SOCKS5_MAX_MSG];
    size_t len = 0;
    msg[len++] = SOCKS5_VERSION;
    msg[len++] = SOCKS5_CMD_CONNECT;
    msg[len++] = 0x00; // reserved
    msg[len++] = SOCKS5_ATYP_DOMAIN;
    msg[len++] = (uint8_t)host_len;
    memcpy(msg + len, dest_host, host_len);
    len += host_len;
    msg[len++] = (uint8_t)(dest_port >> 8);
    msg[len++] = (uint8_t)(dest_port & 0xFF);

    if (write_all(fd, msg, len) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t head[4];
    if (read_all(fd, head, sizeof(head)) != ESP_OK) {
        return ESP_FAIL;
    }
    if (head[0] != SOCKS5_VERSION) {
        ESP_LOGE(TAG, "bad reply version 0x%02x", head[0]);
        return ESP_FAIL;
    }
    if (head[1] != 0x00) {
        ESP_LOGE(TAG, "proxy refused CONNECT to %s:%u: %s (0x%02x)",
                 dest_host, dest_port, reply_error(head[1]), head[1]);
        return ESP_FAIL;
    }

    // The bound address is of no use to us, but it has to be drained off the
    // socket before the tunnel carries application bytes.
    size_t bound_len;
    switch (head[3]) {
    case SOCKS5_ATYP_IPV4:
        bound_len = 4 + 2;
        break;
    case SOCKS5_ATYP_IPV6:
        bound_len = 16 + 2;
        break;
    case SOCKS5_ATYP_DOMAIN: {
        uint8_t name_len;
        if (read_all(fd, &name_len, 1) != ESP_OK) {
            return ESP_FAIL;
        }
        bound_len = (size_t)name_len + 2;
        break;
    }
    default:
        ESP_LOGE(TAG, "proxy replied with unknown address type 0x%02x", head[3]);
        return ESP_FAIL;
    }

    uint8_t discard[18];
    return read_all(fd, discard, bound_len);
}

esp_err_t socks5_handshake(int fd, const char *user, const char *pass,
                            const char *dest_host, uint16_t dest_port)
{
    bool want_auth = (user != NULL && user[0] != '\0' && pass != NULL);

    uint8_t method;
    esp_err_t err = negotiate_method(fd, want_auth, &method);
    if (err != ESP_OK) {
        return err;
    }

    if (method == SOCKS5_AUTH_USERPASS) {
        err = authenticate(fd, user, pass);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = request_connect(fd, dest_host, dest_port);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "tunnel established to %s:%u%s", dest_host, dest_port,
             method == SOCKS5_AUTH_USERPASS ? " (authenticated)" : "");
    return ESP_OK;
}
