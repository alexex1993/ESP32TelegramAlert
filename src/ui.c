#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "display.h"
#include "fonts.h"
#include "msg_queue.h"
#include "secrets.h"

#define COLOR_BG      0x000000
#define COLOR_HEADER  0x7788AA
#define COLOR_SENDER  0x4FC3F7
#define COLOR_TEXT    0xFFFFFF
#define COLOR_FOOTER  0x66788A
#define COLOR_IDLE    0x556677

static lv_obj_t *s_count_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_sender_label;
static lv_obj_t *s_body_label;
static lv_obj_t *s_footer_label;

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

    s_body_label = lv_label_create(scr);
    lv_label_set_text(s_body_label, "");
    // The whole point of the landscape rotation: long Russian sentences wrap
    // across 320px instead of 172px, so a page is a few lines rather than a
    // column of fragments.
    lv_label_set_long_mode(s_body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_body_label, lv_pct(100));
    lv_obj_set_flex_grow(s_body_label, 1);
    lv_obj_set_style_text_font(s_body_label, &pager_font_20, 0);
    lv_obj_set_style_text_color(s_body_label, lv_color_hex(COLOR_TEXT), 0);

    s_footer_label = lv_label_create(scr);
    lv_label_set_text(s_footer_label, "");
    lv_label_set_long_mode(s_footer_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_footer_label, lv_pct(100));
    lv_obj_set_style_text_color(s_footer_label, lv_color_hex(COLOR_FOOTER), 0);

    display_lvgl_unlock();
}

// Telegram stamps every message with its own send time, so the [HH:MM] shown
// is right even when the device clock never got an SNTP answer.
static void format_time(int64_t unix_utc, char *buf, size_t size)
{
    time_t shifted = (time_t)(unix_utc + (int64_t)SECRET_TZ_OFFSET_HOURS * 3600);
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
    if (have) {
        lv_label_set_text(s_sender_label, msg.from);
        lv_label_set_text(s_body_label, msg.text);
        lv_obj_set_style_text_color(s_body_label, lv_color_hex(COLOR_TEXT), 0);
    } else {
        lv_label_set_text(s_sender_label, "");
        lv_label_set_text(s_body_label, "Нет новых сообщений");
        lv_obj_set_style_text_color(s_body_label, lv_color_hex(COLOR_IDLE), 0);
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
