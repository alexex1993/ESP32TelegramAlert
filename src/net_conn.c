#include "net_conn.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "settings.h"
#include "socks5.h"

static const char *TAG = "net";

bool net_conn_uses_proxy(void)
{
    return settings_get()->proxy_enabled;
}

static void set_io_timeouts(int fd, int io_timeout_ms)
{
    struct timeval tv = {
        .tv_sec = io_timeout_ms / 1000,
        .tv_usec = (io_timeout_ms % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// Connects with an explicit timeout. A plain blocking connect() would instead
// follow lwIP's SYN retry schedule, which takes tens of seconds to give up on
// an unreachable proxy.
static esp_err_t tcp_connect(const char *host, uint16_t port, int timeout_ms, int *out_fd)
{
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0 || res == NULL) {
        ESP_LOGE(TAG, "cannot resolve %s (getaddrinfo rc=%d)", host, rc);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ESP_FAIL;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            struct timeval tv = {
                .tv_sec = timeout_ms / 1000,
                .tv_usec = (timeout_ms % 1000) * 1000,
            };
            int sel = select(fd + 1, NULL, &wset, NULL, &tv);
            if (sel > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
                rc = (so_error == 0) ? 0 : -1;
                errno = so_error;
            } else {
                rc = -1;
                errno = (sel == 0) ? ETIMEDOUT : errno;
            }
        }

        if (rc == 0) {
            fcntl(fd, F_SETFL, flags); // back to blocking for the SOCKS5 handshake
            *out_fd = fd;
            err = ESP_OK;
            break;
        }

        ESP_LOGW(TAG, "connect to %s:%u failed: errno %d", host, port, errno);
        close(fd);
    }

    freeaddrinfo(res);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not connect to %s:%u", host, port);
    }
    return err;
}

esp_err_t net_conn_open(const char *host, uint16_t port, int connect_timeout_ms,
                         int io_timeout_ms, int *out_fd)
{
    const app_settings_t *s = settings_get();

    if (s->proxy_enabled) {
        int fd = -1;
        esp_err_t err = tcp_connect(s->proxy_host, s->proxy_port, connect_timeout_ms, &fd);
        if (err != ESP_OK) {
            return err;
        }
        set_io_timeouts(fd, io_timeout_ms);

        // Empty user => RFC 1929 no-auth handshake, same convention as before.
        const char *user = s->proxy_user[0] ? s->proxy_user : NULL;
        const char *pass = s->proxy_pass[0] ? s->proxy_pass : NULL;
        err = socks5_handshake(fd, user, pass, host, port);
        if (err != ESP_OK) {
            close(fd);
            return err;
        }
        *out_fd = fd;
        return ESP_OK;
    }

    int fd = -1;
    esp_err_t err = tcp_connect(host, port, connect_timeout_ms, &fd);
    if (err != ESP_OK) {
        return err;
    }
    set_io_timeouts(fd, io_timeout_ms);
    *out_fd = fd;
    return ESP_OK;
}

esp_err_t net_conn_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "cannot set O_NONBLOCK: errno %d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void net_conn_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}
