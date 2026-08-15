#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "display.h"
#include "fonts.h"
#include "msg_queue.h"
#include "settings.h"
#include "ui_strings.h"

#define COLOR_BG      0x000000
#define COLOR_HEADER  0x7788AA
#define COLOR_SENDER  0x4FC3F7
#define COLOR_TEXT    0xFFFFFF
#define COLOR_FOOTER  0x66788A
#define COLOR_IDLE    0x556677
#define COLOR_SCROLLBAR 0x334455

static lv_obj_t *s_count_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_sender_label;
static lv_obj_t *s_body_view;
static lv_obj_t *s_body_label;
static lv_obj_t *s_footer_label;

// Which message the body is currently showing. ui_render_queue() runs on every
// repaint, including ones that leave the head of the queue alone (a new
// message arriving behind it, say), and restarting the scroll on those would
// yank the text back to the top mid-read. message_id is only unique within a
// chat, so the pair is the identity -- plus the inline id, because a page that
// arrived inline has no chat and only a synthetic message id, which is handed
// out by a counter that restarts at every boot.
static int64_t s_shown_chat_id;
static int64_t s_shown_message_id;
static char s_shown_inline_id[APP_INLINE_MSG_ID_MAX];
static bool s_shown_valid;

void ui_init(lv_display_t *disp)
{
    display_lvgl_lock();

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 6, 0);
    lv_obj_set_style_pad_row(scr, 3, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    // Every child inherits the Cyrillic-capable font from here; LVGL's own
    // default is ASCII-only Montserrat and would render Russian as boxes.
    // This holds whatever UI_LANGUAGE is: the message text below comes from
    // Telegram, not from the string table.
    lv_obj_set_style_text_font(scr, &pager_font_14, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);

    s_count_label = lv_label_create(header);
    lv_label_set_text(s_count_label, LV_SYMBOL_ENVELOPE " 0");
    lv_obj_set_style_text_color(s_count_label, lv_color_hex(COLOR_HEADER), 0);

    s_time_label = lv_label_create(header);
    lv_label_set_text(s_time_label, "");
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(COLOR_HEADER), 0);

    s_sender_label = lv_label_create(scr);
    lv_label_set_text(s_sender_label, "");
    lv_label_set_long_mode(s_sender_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_sender_label, lv_pct(100));
    lv_obj_set_style_text_color(s_sender_label, lv_color_hex(COLOR_SENDER), 0);

    // The body is a viewport plus an over-tall label rather than a label on
    // its own: a label cannot scroll itself, and clipping a long message at
    // the bottom of the glass would hide text with no way to reach it.
    s_body_view = lv_obj_create(scr);
    lv_obj_remove_style_all(s_body_view);
    lv_obj_set_width(s_body_view, lv_pct(100));
    lv_obj_set_flex_grow(s_body_view, 1);
    lv_obj_set_scroll_dir(s_body_view, LV_DIR_VER);
    // AUTO means the bar appears exactly when the content overflows, which is
    // also the only time it says anything: "there is more below".
    lv_obj_set_scrollbar_mode(s_body_view, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_body_view, lv_color_hex(COLOR_SCROLLBAR), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_body_view, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_body_view, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_body_view, 2, LV_PART_SCROLLBAR);

    s_body_label = lv_label_create(s_body_view);
    // Start in the same state ui_render_queue() paints for an empty queue.
    // That call short-circuits when nothing changed, so the empty screen has to
    // be correct before the first one arrives.
    lv_label_set_text(s_body_label, STR_NO_MESSAGES);
    // The whole point of the landscape rotation: long Russian sentences wrap
    // across 320px instead of 172px, so a page is a few lines rather than a
    // column of fragments.
    lv_label_set_long_mode(s_body_label, LV_LABEL_LONG_WRAP);
    // Percent of the viewport, minus room for the scrollbar so the last glyph
    // of a wrapped line is never underneath it.
    lv_obj_set_width(s_body_label, lv_pct(100));
    lv_obj_set_style_pad_right(s_body_label, 6, 0);
    lv_obj_set_height(s_body_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_body_label, &pager_font_20, 0);
    lv_obj_set_style_text_color(s_body_label, lv_color_hex(COLOR_IDLE), 0);

    s_footer_label = lv_label_create(scr);
    lv_label_set_text(s_footer_label, "");
    lv_label_set_long_mode(s_footer_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_footer_label, lv_pct(100));
    lv_obj_set_style_text_color(s_footer_label, lv_color_hex(COLOR_FOOTER), 0);

    display_lvgl_unlock();
}

static void body_scroll_exec(void *view, int32_t y)
{
    lv_obj_scroll_to_y((lv_obj_t *)view, y, LV_ANIM_OFF);
}

// Rewind to the top and, if the new text does not fit, set it crawling. Must
// be called with the LVGL lock held and after the body text has changed.
static void body_restart_scroll(void)
{
    lv_anim_delete(s_body_view, body_scroll_exec);
    lv_obj_scroll_to_y(s_body_view, 0, LV_ANIM_OFF);

    // The label was resized a moment ago and flex has not re-run yet, so the
    // overflow below is still the *previous* message's without this.
    lv_obj_update_layout(s_body_view);
    int32_t overflow = lv_obj_get_scroll_bottom(s_body_view);
    if (overflow <= 0) {
        return;
    }

    uint32_t duration = (uint32_t)overflow * 1000u / UI_BODY_SCROLL_SPEED_PX_S;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_body_view);
    lv_anim_set_exec_cb(&a, body_scroll_exec);
    lv_anim_set_values(&a, 0, overflow);
    lv_anim_set_duration(&a, duration);
    // Linear: a constant crawl is what stays readable. An eased path would
    // sprint through the middle of the message, which is where the text is.
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_delay(&a, UI_BODY_SCROLL_PAUSE_MS);
    lv_anim_set_reverse_delay(&a, UI_BODY_SCROLL_PAUSE_MS);
    lv_anim_set_reverse_duration(&a, UI_BODY_SCROLL_RETURN_MS);
    // Loop rather than stop at the bottom: the pager is glanced at, not
    // watched, so whenever it is picked up the message is still being cycled.
    lv_anim_set_repeat_delay(&a, UI_BODY_SCROLL_PAUSE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

// Telegram stamps every message with its own send time, so the [HH:MM] shown
// is right even when the device clock never got an SNTP answer. The offset is
// a runtime setting now, read through settings_get().
static void format_time(int64_t unix_utc, char *buf, size_t size)
{
    int tz = settings_get()->tz_offset_hours;
    time_t shifted = (time_t)(unix_utc + (int64_t)tz * 3600);
    struct tm tm;
    gmtime_r(&shifted, &tm);
    snprintf(buf, size, "[%02d:%02d]", tm.tm_hour, tm.tm_min);
}

void ui_render_queue(void)
{
    pager_msg_t msg;
    bool have = msg_queue_peek(&msg);
    size_t count = msg_queue_count();
    size_t dropped = msg_queue_dropped();

    char count_buf[48];
    if (dropped > 0) {
        snprintf(count_buf, sizeof(count_buf), LV_SYMBOL_ENVELOPE " %u  " LV_SYMBOL_WARNING " %u",
                 (unsigned)count, (unsigned)dropped);
    } else {
        snprintf(count_buf, sizeof(count_buf), LV_SYMBOL_ENVELOPE " %u", (unsigned)count);
    }

    char time_buf[16] = "";
    if (have) {
        format_time(msg.date, time_buf, sizeof(time_buf));
    }

    display_lvgl_lock();
    lv_label_set_text(s_count_label, count_buf);
    lv_label_set_text(s_time_label, time_buf);

    bool body_changed = (have != s_shown_valid) ||
                        (have && (msg.message_id != s_shown_message_id ||
                                  msg.chat_id != s_shown_chat_id ||
                                  strcmp(msg.inline_message_id, s_shown_inline_id) != 0));
    if (body_changed) {
        if (have) {
            lv_label_set_text(s_sender_label, msg.from);
            lv_label_set_text(s_body_label, msg.text);
            lv_obj_set_style_text_color(s_body_label, lv_color_hex(COLOR_TEXT), 0);
        } else {
            lv_label_set_text(s_sender_label, "");
            lv_label_set_text(s_body_label, STR_NO_MESSAGES);
            lv_obj_set_style_text_color(s_body_label, lv_color_hex(COLOR_IDLE), 0);
        }
        body_restart_scroll();
        s_shown_valid = have;
        s_shown_chat_id = have ? msg.chat_id : 0;
        s_shown_message_id = have ? msg.message_id : 0;
        snprintf(s_shown_inline_id, sizeof(s_shown_inline_id), "%s",
                 have ? msg.inline_message_id : "");
    }
    display_lvgl_unlock();
}

void ui_set_status(const char *text)
{
    display_lvgl_lock();
    if (s_footer_label) {
        lv_label_set_text(s_footer_label, text);
    }
    display_lvgl_unlock();
}

void ui_set_statusf(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_set_status(buf);
}

void ui_show_provision(const char *ap_ssid, const char *url)
{
    // Build the body as one string so it wraps cleanly under the same flex
    // layout the pager body uses. Three lines: the two numbered steps and a
    // blank line for breathing room. The AP name and URL are short enough to
    // fit a 320px-wide landscape row in the 20px font.
    char body[160];
    snprintf(body, sizeof(body), "%s\n%s\n\n%s",
             STR_PROVISION_AP_LABEL, ap_ssid, url);

    display_lvgl_lock();
    // Clear the header so the empty-queue envelope icon does not sit next to
    // the setup instructions.
    if (s_count_label) {
        char hdr[48];
        snprintf(hdr, sizeof(hdr), "%s %s", LV_SYMBOL_WIFI, STR_PROVISION_TITLE);
        lv_label_set_text(s_count_label, hdr);
    }
    if (s_time_label) {
        lv_label_set_text(s_time_label, "");
    }
    if (s_sender_label) {
        lv_label_set_text(s_sender_label, "");
    }
    // Stop any scroll animation left over from a prior message so the URL
    // stays put, then paint the two-step instruction.
    lv_anim_delete(s_body_view, body_scroll_exec);
    lv_obj_scroll_to_y(s_body_view, 0, LV_ANIM_OFF);
    lv_label_set_text(s_body_label, body);
    lv_obj_set_style_text_color(s_body_label, lv_color_hex(COLOR_TEXT), 0);
    if (s_footer_label) {
        lv_label_set_text(s_footer_label, STR_PROVISION_URL_LABEL);
    }
    display_lvgl_unlock();
}
