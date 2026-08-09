#pragma once

#include <stdint.h>

#include "esp_err.h"

// Performs the SOCKS5 client handshake (RFC 1928) on an already-connected
// socket to the proxy, optionally authenticating with username/password
// (RFC 1929), and asks the proxy to CONNECT to dest_host:dest_port.
//
// The destination is sent as a domain name rather than an address, so name
// resolution happens on the proxy side. That matters here: the usual reason
// to put a proxy in front of api.telegram.org is that the local network
// cannot reach -- or cannot honestly resolve -- it.
//
// Leave `user` NULL or empty for an unauthenticated proxy. On success the
// socket is a transparent tunnel to the destination and is ready for TLS.
esp_err_t socks5_handshake(int fd, const char *user, const char *pass,
                            const char *dest_host, uint16_t dest_port);
