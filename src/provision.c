#include "provision.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "settings.h"
#include "ui.h"
#include "ui_strings.h"
#include "wifi_manager.h"

static const char *TAG = "provision";

// ---------------------------------------------------------------- DNS hijack
// A tiny UDP/53 responder that answers *every* A query with the AP's own
// address. That is what makes a phone that joined the open AP pop the
// captive-portal sign-in sheet: the OS probes a well-known URL
// (clients3.google.com/generate_204, captive.apple.com/hotspot-detect.html,
// msftconnecttest.com/redirect, ...), the probe resolves to the ESP, the HTTP
// server 302-redirects it to "/", and the OS opens that page in the captive
// browser. Without this the URL printed on the LCD still works by hand.

static void dns_hijack_task(void *arg)
{
    uint8_t ip_bytes[4] = { 192, 168, 4, 1 };
    {
        // Resolve the AP address from the string form so a future change to the
        // AP IP needs no edit here.
        char ipstr[24];
        wifi_manager_get_ap_ip(ipstr, sizeof(ipstr));
        unsigned a, b, c, d;
        if (sscanf(ipstr, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            ip_bytes[0] = (uint8_t)a; ip_bytes[1] = (uint8_t)b;
            ip_bytes[2] = (uint8_t)c; ip_bytes[3] = (uint8_t)d;
        }
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns: socket() failed");
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(53);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "dns: bind(:53) failed errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "dns hijack listening on :53 -> %u.%u.%u.%u",
             ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3]);

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (n < 12) {
            continue;   // smaller than a DNS header -- not a real query
        }
        // Only answer standard queries with exactly one question; anything else
        // (response, multi-question, truncated) is dropped to stay simple and
        // safe -- the captive probes are all single-question.
        uint16_t flags = ((uint16_t)buf[2] << 8) | buf[3];
        uint16_t qdcount = ((uint16_t)buf[4] << 8) | buf[5];
        if ((flags & 0x8000) || qdcount != 1) {
            continue;
        }

        // Walk one QNAME to find the end of the question section.
        int off = 12;
        while (off < n) {
            uint8_t len = buf[off];
            if (len == 0) {
                off++;      // root terminator
                break;
            }
            if ((len & 0xC0) == 0xC0) {
                off += 2;   // compression pointer (unexpected in a query)
                break;
            }
            off += 1 + len;
        }
        off += 4;   // QTYPE (2) + QCLASS (2)

        // Need room for: name-ptr(2) TYPE(2) CLASS(2) TTL(4) RDLEN(2) RDATA(4) = 16
        if (off + 16 > (int)sizeof(buf)) {
            continue;
        }

        // Turn it into a response: QR=1, opcode 0, AA=1, RA=1; ANCOUNT=1.
        buf[2] |= 0x80;    // QR
        buf[2] |= 0x04;    // AA
        buf[3] |= 0x80;    // RA
        buf[6] = 0; buf[7] = 1;   // ANCOUNT = 1

        int a = off;
        buf[a++] = 0xC0; buf[a++] = 0x0C;              // name ptr -> offset 12
        buf[a++] = 0; buf[a++] = 1;                    // TYPE A
        buf[a++] = 0; buf[a++] = 1;                    // CLASS IN
        buf[a++] = 0; buf[a++] = 0; buf[a++] = 0; buf[a++] = 60;  // TTL 60s
        buf[a++] = 0; buf[a++] = 4;                    // RDLENGTH 4
        buf[a++] = ip_bytes[0];
        buf[a++] = ip_bytes[1];
        buf[a++] = ip_bytes[2];
        buf[a++] = ip_bytes[3];

        sendto(sock, buf, a, 0, (struct sockaddr *)&src, slen);
    }
}

// ----------------------------------------------------------- form parsing
// URL-decode in place (src == dst allowed). Returns the new length; the buffer
// is NUL-terminated. '+' becomes space, %XX is the byte.
static size_t url_decode(char *buf, size_t cap)
{
    size_t r = 0, w = 0;
    while (r < cap && buf[r] && w + 1 < cap) {
        char c = buf[r];
        if (c == '+') {
            buf[w++] = ' ';
            r++;
        } else if (c == '%' && r + 2 < cap && isxdigit((unsigned char)buf[r + 1]) && isxdigit((unsigned char)buf[r + 2])) {
            int hi = buf[r + 1] <= '9' ? buf[r + 1] - '0' : (tolower(buf[r + 1]) - 'a' + 10);
            int lo = buf[r + 2] <= '9' ? buf[r + 2] - '0' : (tolower(buf[r + 2]) - 'a' + 10);
            buf[w++] = (char)((hi << 4) | lo);
            r += 3;
        } else {
            buf[w++] = c;
            r++;
        }
    }
    buf[w] = '\0';
    return w;
}

// Copy the value for `key` out of an application/x-www-form-urlencoded body into
// `dst` (NUL-terminated, url-decoded). Unknown key -> dst untouched.
static void form_get(const char *body, const char *key, char *dst, size_t dst_size)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        const char *val = eq ? eq + 1 : p;
        size_t keyend = eq ? (size_t)(eq - p) : (amp ? (size_t)(amp - p) : strlen(p));

        if (keyend == klen && strncmp(p, key, klen) == 0) {
            size_t vlen = amp ? (size_t)(amp - val) : strlen(val);
            if (vlen >= dst_size) {
                vlen = dst_size - 1;
            }
            char tmp[256];
            if (vlen >= sizeof(tmp)) {
                vlen = sizeof(tmp) - 1;
            }
            memcpy(tmp, val, vlen);
            tmp[vlen] = '\0';
            url_decode(tmp, sizeof(tmp));
            strlcpy(dst, tmp, dst_size);
            return;
        }
        if (!amp) {
            break;
        }
        p = amp + 1;
    }
}

static long form_get_long(const char *body, const char *key, long fallback)
{
    char tmp[32];
    tmp[0] = '\0';
    form_get(body, key, tmp, sizeof(tmp));
    if (tmp[0] == '\0') {
        return fallback;
    }
    return strtol(tmp, NULL, 10);
}

// Parses + validates the POST body into `out`. On success returns true and the
// caller persists `out`. On failure fills `errbuf` with a human message.
static bool parse_and_validate(const char *body, app_settings_t *out, char *errbuf, size_t errbuf_sz)
{
    memset(out, 0, sizeof(*out));
    form_get(body, "bot_token", out->bot_token, sizeof(out->bot_token));
    // WiFi: up to three networks. Slot order is the try order at boot; a slot
    // whose SSID is empty is ignored entirely (its password, if any, is
    // dropped), and the filled slots are compacted to the front because the
    // connect loop walks until the first empty SSID.
    for (int i = 0; i < APP_SETTINGS_WIFI_NETS_MAX; i++) {
        char key[16];
        snprintf(key, sizeof(key), "wifi_ssid_%d", i + 1);
        form_get(body, key, out->wifi_ssid[i], sizeof(out->wifi_ssid[i]));
        snprintf(key, sizeof(key), "wifi_password_%d", i + 1);
        form_get(body, key, out->wifi_password[i], sizeof(out->wifi_password[i]));
        if (out->wifi_ssid[i][0] == '\0') {
            out->wifi_password[i][0] = '\0';
        }
    }
    for (int i = 0; i < APP_SETTINGS_WIFI_NETS_MAX; i++) {
        if (out->wifi_ssid[i][0] != '\0') {
            continue;
        }
        for (int j = i + 1; j < APP_SETTINGS_WIFI_NETS_MAX; j++) {
            if (out->wifi_ssid[j][0] != '\0') {
                strlcpy(out->wifi_ssid[i], out->wifi_ssid[j], sizeof(out->wifi_ssid[i]));
                strlcpy(out->wifi_password[i], out->wifi_password[j], sizeof(out->wifi_password[i]));
                out->wifi_ssid[j][0] = '\0';
                out->wifi_password[j][0] = '\0';
                break;
            }
        }
    }
    form_get(body, "proxy_host", out->proxy_host, sizeof(out->proxy_host));
    form_get(body, "proxy_user", out->proxy_user, sizeof(out->proxy_user));
    form_get(body, "proxy_pass", out->proxy_pass, sizeof(out->proxy_pass));
    out->tz_offset_hours = (int32_t)form_get_long(body, "tz_offset", APP_DEFAULT_TZ_OFFSET_HOURS);

    {
        char lang[8];
        lang[0] = '\0';
        form_get(body, "language", lang, sizeof(lang));
        out->ui_language = (strcmp(lang, "russian") == 0) ? APP_LANG_RU : APP_LANG_EN;
    }
    {
        char ptype[16];
        ptype[0] = '\0';
        form_get(body, "proxy_type", ptype, sizeof(ptype));
        out->proxy_enabled = (strcmp(ptype, "socks5") == 0);
    }
    if (out->proxy_enabled) {
        out->proxy_port = (uint16_t)form_get_long(body, "proxy_port", 0);
    }

    // Validation -- mirror tools/gen_secrets.py's old rules so the portal is
    // not a way to store a half-config that the runtime would choke on.
    if (out->bot_token[0] == '\0' || strchr(out->bot_token, ':') == NULL) {
        snprintf(errbuf, errbuf_sz, "Bot token looks wrong (expected digits:letters).");
        return false;
    }
    if (out->wifi_ssid[0][0] == '\0') {
        snprintf(errbuf, errbuf_sz, "WiFi network name is required.");
        return false;
    }
    if (out->tz_offset_hours < -12 || out->tz_offset_hours > 14) {
        snprintf(errbuf, errbuf_sz, "Timezone offset must be between -12 and 14.");
        return false;
    }
    if (out->proxy_enabled) {
        if (out->proxy_host[0] == '\0' || out->proxy_port == 0) {
            snprintf(errbuf, errbuf_sz, "SOCKS5 needs a host and a port.");
            return false;
        }
        // RFC 1929: user/pass both set or both empty.
        if ((out->proxy_user[0] == '\0') != (out->proxy_pass[0] == '\0')) {
            snprintf(errbuf, errbuf_sz, "Set proxy user and password together, or neither.");
            return false;
        }
    }
    return true;
}

// ----------------------------------------------------------- HTML page
// Append helper for building the page into one malloc'd buffer.
struct buf {
    char *p;
    size_t cap, len;
};

static void buf_put(struct buf *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 2048;
        while (b->len + n + 1 > ncap) {
            ncap *= 2;
        }
        char *np = realloc(b->p, ncap);
        if (!np) {
            return;     // out of memory: the page will be truncated, but safe
        }
        b->p = np;
        b->cap = ncap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void buf_printf(struct buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        buf_put(b, tmp);
    }
}

// HTML-escape `src` into `dst` for attribute/text contexts.
static void html_escape(char *dst, size_t dst_size, const char *src)
{
    size_t w = 0;
    if (dst_size == 0) {
        return;
    }
    for (const char *p = src; *p && w + 7 < dst_size; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '&':  memcpy(dst + w, "&amp;", 5);  w += 5; break;
        case '<':  memcpy(dst + w, "&lt;", 4);   w += 4; break;
        case '>':  memcpy(dst + w, "&gt;", 4);   w += 4; break;
        case '"':  memcpy(dst + w, "&quot;", 6); w += 6; break;
        case '\'': memcpy(dst + w, "&#39;", 5);  w += 5; break;
        default:
            dst[w++] = (char)c;
            break;
        }
    }
    dst[w] = '\0';
}

#define ESC(buf, src) html_escape((buf), sizeof(buf), (src))

// Builds the HTML page, returning a malloc'd NUL-terminated string the caller
// frees. `err` is an optional error banner; `s` provides the pre-fill values.
static char *build_page(const char *err, const app_settings_t *s)
{
    char e_tok[200], e_ph[160], e_pu[120], e_pp[120];
    char e_ssid[APP_SETTINGS_WIFI_NETS_MAX][80];
    char e_pass[APP_SETTINGS_WIFI_NETS_MAX][80];
    ESC(e_tok, s->bot_token);
    for (int i = 0; i < APP_SETTINGS_WIFI_NETS_MAX; i++) {
        ESC(e_ssid[i], s->wifi_ssid[i]);
        ESC(e_pass[i], s->wifi_password[i]);
    }
    ESC(e_ph, s->proxy_host);
    ESC(e_pu, s->proxy_user);
    ESC(e_pp, s->proxy_pass);

    struct buf b = {0};
    buf_put(&b,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Telegram Pager setup</title><style>"
        "body{font-family:system-ui,sans-serif;background:#101418;color:#e6e9ee;"
        "margin:0;padding:16px;max-width:480px}"
        "h1{font-size:20px;margin:8px 0 4px}"
        ".muted{color:#8a94a6;font-size:13px;margin:0 0 14px}"
        "label{display:block;font-size:13px;margin:12px 0 4px;color:#a9b3c0}"
        "input,select{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;"
        "border:1px solid #2c3540;background:#171c22;color:#e6e9ee;font-size:15px}"
        "input:focus,select:focus{outline:none;border-color:#4f7cc4}"
        ".row2{display:flex;gap:10px}.row2>div{flex:1}"
        "button{width:100%;margin-top:18px;padding:13px;border:0;border-radius:8px;"
        "background:#2e6df6;color:#fff;font-size:16px;font-weight:600}"
        ".err{background:#3a1d1d;color:#ffb4b4;padding:10px 12px;border-radius:8px;"
        "margin:8px 0;font-size:14px}"
        ".proxy{display:none}"
        "</style></head><body>"
        "<h1>Telegram Pager</h1>"
        "<p class=muted>First-time setup. Fill in the fields and the pager will "
        "reboot and connect.</p>");

    if (err && err[0]) {
        buf_printf(&b, "<div class=err>%s</div>", err);
    }

    buf_put(&b, "<form method=post action=/save autocomplete=off>");
    buf_printf(&b, "<label>Bot token</label>"
                   "<input name=bot_token value=\"%s\" placeholder=\"123456:ABC-DEF...\">", e_tok);

    buf_printf(&b, "<label>WiFi network 1 (primary)</label>"
                   "<input name=wifi_ssid_1 value=\"%s\">", e_ssid[0]);
    buf_printf(&b, "<label>WiFi password</label>"
                   "<input name=wifi_password_1 value=\"%s\" placeholder=\"(leave empty for open)\">", e_pass[0]);
    buf_printf(&b, "<label>WiFi network 2 (optional)</label>"
                   "<input name=wifi_ssid_2 value=\"%s\">", e_ssid[1]);
    buf_printf(&b, "<label>WiFi password</label>"
                   "<input name=wifi_password_2 value=\"%s\" placeholder=\"(leave empty for open)\">", e_pass[1]);
    buf_printf(&b, "<label>WiFi network 3 (optional)</label>"
                   "<input name=wifi_ssid_3 value=\"%s\">", e_ssid[2]);
    buf_printf(&b, "<label>WiFi password</label>"
                   "<input name=wifi_password_3 value=\"%s\" placeholder=\"(leave empty for open)\">", e_pass[2]);
    buf_put(&b, "<p class=muted>If a network is unreachable, the pager tries the "
                "next one on this list.</p>");

    buf_printf(&b, "<div class=row2><div><label>Timezone (UTC offset, hours)</label>"
                   "<input name=tz_offset type=number value=\"%d\" min=-12 max=14></div>", (int)s->tz_offset_hours);
    buf_printf(&b, "<div><label>Language</label><select name=language>"
                   "<option value=english%s>English</option>"
                   "<option value=russian%s>Русский</option>"
                   "</select></div></div>",
               s->ui_language == APP_LANG_EN ? " selected" : "",
               s->ui_language == APP_LANG_RU ? " selected" : "");

    buf_printf(&b, "<label>Proxy</label><select name=proxy_type id=ptype onchange=tog()>"
                   "<option value=none%s>None (direct)</option>"
                   "<option value=socks5%s>SOCKS5</option></select>",
               s->proxy_enabled ? "" : " selected",
               s->proxy_enabled ? " selected" : "");

    buf_put(&b, "<div class=proxy id=pdiv>");
    buf_printf(&b, "<div class=row2><div><label>Proxy host</label>"
                   "<input name=proxy_host value=\"%s\"></div>", e_ph);
    buf_printf(&b, "<div><label>Proxy port</label>"
                   "<input name=proxy_port type=number value=\"%u\"></div></div>", s->proxy_port);
    buf_printf(&b, "<div class=row2><div><label>Proxy user (optional)</label>"
                   "<input name=proxy_user value=\"%s\"></div>", e_pu);
    buf_printf(&b, "<div><label>Proxy password (optional)</label>"
                   "<input name=proxy_pass value=\"%s\"></div></div>", e_pp);
    buf_put(&b, "</div>");

    buf_put(&b, "<button type=submit>Save &amp; reboot</button></form>");

    buf_put(&b,
        "<script>"
        "function tog(){var p=document.getElementById('ptype').value==\"socks5\";"
        "document.getElementById('pdiv').style.display=p?\"block\":\"none\"}"
        "tog();"
        "</script>");
    buf_put(&b, "</body></html>");

    return b.p;
}

// ----------------------------------------------------------- HTTP handlers

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const app_settings_t *s = settings_get();
    char *page = build_page(NULL, s);
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    // The form is small, but cap what we accept at a sane bound and read it in
    // one shot (httpd_req_recv loops internally on partial reads up to the
    // requested length when the body fits; large bodies are truncated, which
    // then fails validation and re-renders the form). 3072 leaves headroom for
    // three fully percent-encoded SSID/password pairs plus the proxy fields.
    static char body[3072];
    int total = (int)req->content_len;
    if (total < 0) {
        total = 0;
    }
    if ((size_t)total > sizeof(body) - 1) {
        total = sizeof(body) - 1;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, (size_t)(total - received));
        if (r <= 0) {
            break;
        }
        received += r;
    }
    body[received] = '\0';

    app_settings_t parsed;
    char err[160] = "";
    if (!parse_and_validate(body, &parsed, err, sizeof(err))) {
        char *page = build_page(err, &parsed);
        if (page) {
            httpd_resp_set_type(req, "text/html; charset=utf-8");
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
            free(page);
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        }
        return ESP_OK;
    }

    settings_save(&parsed);
    ESP_LOGI(TAG, "settings saved from portal, rebooting");

    // Tell the LCD the save happened, then push a tiny page back so the phone
    // shows something instead of a blank tab.
    ui_set_status(STR_PROVISION_SAVED);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Saved</title>"
        "<body style=\"font-family:system-ui;background:#101418;color:#e6e9ee;"
        "display:flex;height:100vh;margin:0;align-items:center;justify-content:center\">"
        "<div style=\"text-align:center\"><h2>Saved</h2>"
        "<p>Rebooting and connecting to WiFi...</p></div></body></html>",
        HTTPD_RESP_USE_STRLEN);

    // Let the response flush, then reboot into station mode. The force-ap flag
    // was cleared by settings_save(), so the next boot connects straight away.
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;   // unreachable
}

// Catch-all: anything that is not "/" or "/save" (the captive-portal probes
// from iOS/Android/Windows) is redirected to the form.
static esp_err_t redirect_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ----------------------------------------------------------- entry point

void provision_start(void)
{
    ESP_LOGI(TAG, "starting provisioning AP");

    wifi_manager_start_ap(APP_PROVISION_AP_SSID);

    char ipstr[24];
    wifi_manager_get_ap_ip(ipstr, sizeof(ipstr));

    // Paint the on-screen instructions: SSID + the URL to open. The backlight
    // is already on (display_init leaves it lit and the screen sleep countdown
    // has not started yet -- that only begins in pager_start).
    char url[40];
    snprintf(url, sizeof(url), "http://%s", ipstr);
    ui_show_provision(APP_PROVISION_AP_SSID, url);
    ui_set_statusf("%s  http://%s", STR_PROVISION_URL_LABEL, ipstr);

    // DNS hijack so the captive-portal probe resolves here. Tiny stack: the
    // task only recvfrom's and assembles a 16-byte answer suffix.
    xTaskCreate(dns_hijack_task, "dns_hijack", 3072, NULL, 4, NULL);

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.stack_size = 6144;
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    static const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get_handler
    };
    static const httpd_uri_t save = {
        .uri = "/save", .method = HTTP_POST, .handler = save_post_handler
    };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_404_handler);

    ESP_LOGI(TAG, "portal ready: join \"%s\" then open http://%s", APP_PROVISION_AP_SSID, ipstr);
    // The AP + DNS + HTTP server now run independently; block here so app_main
    // does not fall through and start the pager task on top of the portal.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
