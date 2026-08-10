#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Opens a TCP connection to host:port, tunnelled through the SOCKS5 proxy
// configured at runtime (via the provisioning portal) when one is enabled.
// Either way the caller gets back a socket that behaves like a direct
// connection to the destination.
//
// The returned socket is blocking, with send/receive timeouts applied.
esp_err_t net_conn_open(const char *host, uint16_t port, int connect_timeout_ms,
                         int io_timeout_ms, int *out_fd);

// Switches the socket to non-blocking, which is what mbedTLS needs in order
// to report WANT_READ/WANT_WRITE instead of failing on a timed-out read.
esp_err_t net_conn_set_nonblocking(int fd);

void net_conn_close(int fd);

// True when a proxy is configured, for status display.
bool net_conn_uses_proxy(void);
