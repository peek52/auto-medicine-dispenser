#include "ui_core.h"
#include "ds3231.h"
#include "dispenser_scheduler.h"
#include "i2c_manager.h"
#include "dfplayer.h"
#include "offline_sync.h"
#include "wifi_sta.h"
#include "ui_standby_thai_labels.h"
#include "ui_utf8_text.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static bool s_today_schedule_popup_drawn = false;
static bool s_hw_alert_drawn = false;
static uint8_t s_hw_alert_mask = 0;
static uint32_t s_hw_health_next_check_ms = 0;
static bool s_cached_pca_err = false;
static bool s_cached_pcf_err = false;
static bool s_cached_rtc_err = false;
static int s_schedule_visible_slots[7] = {0};
static int s_schedule_visible_count = 0;
static int s_schedule_detail_slot = -1;
static bool s_show_only_missed = false;

static const int kSchedulePopupX = 24;
static const int kSchedulePopupY = 24;
static const int kSchedulePopupW = 432;
static const int kSchedulePopupH = 272;
static const int kScheduleCloseX = 390;
static const int kScheduleCloseY = 36;
static const int kScheduleCloseW = 58;
static const int kScheduleCloseH = 32;
static const int kScheduleHeaderCenterX = 206;
static const int kScheduleHeaderTextX = 56;
static const int kScheduleHeaderTextW = 304;
static const int kScheduleRowX = 42;
static const int kScheduleRowY = 102;
static const int kScheduleRowW = 396;
static const int kScheduleRowH = 27;
static const int kAlertPopupX = 28;
static const int kAlertPopupY = 42;
static const int kAlertPopupW = 424;
static const int kAlertPopupH = 214;
static const int kAlertButtonX = 172;
static const int kAlertButtonY = 206;
static const int kAlertButtonW = 136;
static const int kAlertButtonH = 40;

static void draw_standby_label(int16_t x, int16_t y, const ui_label_bitmap_t *label)
{
    if (!label || !label->pixels) return;
    for (int16_t row = 0; row < label->height; ++row) {
        const uint16_t *src = label->pixels + (row * label->width);
        int16_t run_start = -1;
        for (int16_t col = 0; col <= label->width; ++col) {
            bool opaque = false;
            if (col < label->width) {
                uint16_t px = src[col];
                opaque = (px != THEME_PANEL && px != THEME_CARD && px != THEME_WARN &&
                          px != THEME_OK && px != THEME_BAD && px != ST_RGB565(30, 160, 240) &&
                          px != ST_RGB565(34, 197, 94) && px != ST_RGB565(150, 150, 150));
            }
            if (opaque && run_start < 0) run_start = col;
            else if (!opaque && run_start >= 0) {
                ui_draw_rgb_bitmap(x + run_start, y + row, col - run_start, 1, src + run_start);
                run_start = -1;
            }
        }
    }
}

static void draw_standby_label_centered(int16_t center_x, int16_t y, const ui_label_bitmap_t *label)
{
    if (!label) return;
    draw_standby_label(center_x - (label->width / 2), y, label);
}

static void draw_standby_modal_button(int16_t x, int16_t y, int16_t w, int16_t h,
                                      uint16_t fill, uint16_t border)
{
    fill_round_rect_frame(x, y, w, h, 10, fill, border);
    draw_string_centered(x + (w / 2), y + 27, "OK", 0xFFFF, fill, &FreeSans12pt7b);
}

static const char *standby_translate_slot_th(const char *slot_label)
{
    if (!slot_label) return "";
    if (strcmp(slot_label, "Before Breakfast") == 0) return "ก่อนเช้า";
    if (strcmp(slot_label, "After Breakfast") == 0)  return "หลังเช้า";
    if (strcmp(slot_label, "Before Lunch") == 0)     return "ก่อนกลางวัน";
    if (strcmp(slot_label, "After Lunch") == 0)      return "หลังกลางวัน";
    if (strcmp(slot_label, "Before Dinner") == 0)    return "ก่อนเย็น";
    if (strcmp(slot_label, "After Dinner") == 0)     return "หลังเย็น";
    if (strcmp(slot_label, "Bedtime") == 0)          return "ก่อนนอน";
    return slot_label;
}

static bool standby_build_next_dose_th(const char *dose, char *line1, size_t line1_len, char *line2, size_t line2_len)
{
    if (!dose || !line1 || !line2 || line1_len == 0 || line2_len == 0) return false;
    line1[0] = '\0';
    line2[0] = '\0';

    if (strcmp(dose, "No schedule") == 0 || dose[0] == '\0') {
        return false;
    }

    const char *time_ptr = strrchr(dose, ' ');
    if (!time_ptr) {
        safe_copy(line1, line1_len, dose);
        return true;
    }

    while (time_ptr > dose && *(time_ptr - 1) == ' ') {
        --time_ptr;
    }

    const char *label_end = time_ptr;
    while (label_end > dose && *(label_end - 1) == ' ') {
        --label_end;
    }

    char slot_label[32] = "";
    size_t slot_len = (size_t)(label_end - dose);
    if (slot_len >= sizeof(slot_label)) slot_len = sizeof(slot_label) - 1;
    memcpy(slot_label, dose, slot_len);
    slot_label[slot_len] = '\0';

    safe_copy(line1, line1_len, standby_translate_slot_th(slot_label));
    safe_copy(line2, line2_len, time_ptr);
    return true;
}

static bool standby_build_next_dose_th_single_line(const char *dose, char *line, size_t line_len)
{
    char slot[40] = "";
    char time[16] = "";
    if (!standby_build_next_dose_th(dose, slot, sizeof(slot), time, sizeof(time))) return false;
    // Thai: drop the "น." suffix — looks cluttered after the slot label.
    snprintf(line, line_len, "%s %s", slot, time);
    return true;
}

static bool standby_build_next_dose_en(const char *dose, char *line1, size_t line1_len, char *line2, size_t line2_len)
{
    if (!dose || !line1 || !line2 || line1_len == 0 || line2_len == 0) return false;
    line1[0] = '\0';
    line2[0] = '\0';

    if (strcmp(dose, "No schedule") == 0 || dose[0] == '\0') {
        return false;
    }

    const char *time_ptr = strrchr(dose, ' ');
    if (!time_ptr) {
        safe_copy(line1, line1_len, dose);
        return true;
    }

    while (time_ptr > dose && *(time_ptr - 1) == ' ') {
        --time_ptr;
    }

    const char *label_end = time_ptr;
    while (label_end > dose && *(label_end - 1) == ' ') {
        --label_end;
    }

    size_t slot_len = (size_t)(label_end - dose);
    if (slot_len >= line1_len) slot_len = line1_len - 1;
    memcpy(line1, dose, slot_len);
    line1[slot_len] = '\0';
    safe_copy(line2, line2_len, time_ptr);
    return true;
}

static void standby_split_dose_slot_en(const char *slot_label, char *phase, size_t phase_len,
                                       char *meal, size_t meal_len)
{
    if (!phase || phase_len == 0 || !meal || meal_len == 0) return;

    phase[0] = '\0';
    meal[0] = '\0';
    if (!slot_label || !slot_label[0]) return;

    if (strcmp(slot_label, "Before Breakfast") == 0) {
        safe_copy(phase, phase_len, "Before");
        safe_copy(meal, meal_len, "Breakfast");
        return;
    }
    if (strcmp(slot_label, "After Breakfast") == 0) {
        safe_copy(phase, phase_len, "After");
        safe_copy(meal, meal_len, "Breakfast");
        return;
    }
    if (strcmp(slot_label, "Before Lunch") == 0) {
        safe_copy(phase, phase_len, "Before");
        safe_copy(meal, meal_len, "Lunch");
        return;
    }
    if (strcmp(slot_label, "After Lunch") == 0) {
        safe_copy(phase, phase_len, "After");
        safe_copy(meal, meal_len, "Lunch");
        return;
    }
    if (strcmp(slot_label, "Before Dinner") == 0) {
        safe_copy(phase, phase_len, "Before");
        safe_copy(meal, meal_len, "Dinner");
        return;
    }
    if (strcmp(slot_label, "After Dinner") == 0) {
        safe_copy(phase, phase_len, "After");
        safe_copy(meal, meal_len, "Dinner");
        return;
    }

    safe_copy(meal, meal_len, slot_label);
}

static void draw_standby_next_dose_en_line(int16_t content_left, int16_t content_right, int16_t baseline_y,
                                           const char *phase, const char *meal, const char *time,
                                           uint16_t phase_color, uint16_t main_color, uint16_t bg)
{
    char meal_time[48] = "";
    int16_t phase_w = 0;
    int16_t main_w = 0;
    int16_t gap = 0;
    int16_t total_w = 0;
    int16_t start_x = 0;

    if (!meal || !meal[0]) return;

    if (time && time[0]) {
        // English: append " hrs" so the line reads "Breakfast 08:00 hrs".
        snprintf(meal_time, sizeof(meal_time), "%s %s hrs", meal, time);
    } else {
        safe_copy(meal_time, sizeof(meal_time), meal);
    }

    if (phase && phase[0]) {
        phase_w = gfx_text_width(phase, &FreeSans9pt7b);
        gap = 10;
    }
    main_w = gfx_text_width_scaled(meal_time, &FreeSans12pt7b, 2);
    total_w = phase_w + gap + main_w;

    start_x = content_left + (((content_right - content_left) - total_w) / 2);
    if (start_x < content_left) start_x = content_left;

    fill_rect(content_left, baseline_y - 30, content_right - content_left, 40, bg);

    if (phase_w > 0) {
        draw_string_gfx(start_x, baseline_y - 4, phase, phase_color, bg, &FreeSans9pt7b);
    }
    draw_string_gfx_scaled(start_x + phase_w + gap, baseline_y, meal_time, main_color, bg, &FreeSans12pt7b, 2);
}

static const char *standby_slot_label_en_compact(int slot_idx)
{
    static const char *kCompactLabels[7] = {
        "Before Breakfast", "After Breakfast", "Before Lunch", "After Lunch",
        "Before Dinner", "After Dinner", "Bedtime"
    };
    return (slot_idx >= 0 && slot_idx < 7) ? kCompactLabels[slot_idx] : "";
}

static const char *standby_slot_label_th_compact(int slot_idx)
{
    static const char *kCompactLabels[7] = {
        "เช้า-ก่อน", "เช้า-หลัง", "กลางวัน-ก่อน", "กลางวัน-หลัง",
        "เย็น-ก่อน", "เย็น-หลัง", "ก่อนนอน"
    };
    return (slot_idx >= 0 && slot_idx < 7) ? kCompactLabels[slot_idx] : "";
}

static const char *standby_slot_label_popup(int slot_idx, ui_language_t lang)
{
    if (lang == UI_LANG_TH) return standby_slot_label_th_compact(slot_idx);
    return standby_slot_label_en_compact(slot_idx);
}

static bool standby_slot_time_valid(const char *hhmm)
{
    return hhmm && strlen(hhmm) == 5 && hhmm[2] == ':';
}

static bool standby_parse_hhmm(const char *hhmm, int *hour, int *minute)
{
    if (!standby_slot_time_valid(hhmm) || !hour || !minute) return false;

    int hh = 0, mm = 0;
    if (sscanf(hhmm, "%d:%d", &hh, &mm) != 2) return false;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;

    *hour = hh;
    *minute = mm;
    return true;
}

static int standby_time_to_minutes(const char *hhmm)
{
    int hh = 0, mm = 0;
    if (!standby_parse_hhmm(hhmm, &hh, &mm)) return -1;
    return (hh * 60) + mm;
}

static int standby_slot_assigned_med_count(int slot_idx, bool include_empty_stock)
{
    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh || !sh->loaded || !sh->enabled || slot_idx < 0 || slot_idx >= 7) return 0;

    int count = 0;
    for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
        if (!((sh->med[i].slots >> slot_idx) & 0x01)) continue;
        if (!include_empty_stock && sh->med[i].count <= 0) continue;
        ++count;
    }
    return count;
}

static int standby_collect_today_schedule_slots(int slots[], int max_slots)
{
    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh || !sh->loaded || !sh->enabled || !slots || max_slots <= 0) return 0;

    int count = 0;
    for (int s = 0; s < 7 && count < max_slots; ++s) {
        if (!standby_slot_time_valid(sh->slot_time[s])) continue;
        if (standby_slot_assigned_med_count(s, true) <= 0) continue;
        slots[count++] = s;
    }
    return count;
}

static void standby_build_slot_status(int slot_idx, int now_minutes, ui_language_t lang,
                                      char *buf, size_t buf_len, uint16_t *color)
{
    if (!buf || buf_len == 0) return;
    buf[0] = '\0';

    if (color) *color = THEME_TXT_MAIN;

    int assigned_count = standby_slot_assigned_med_count(slot_idx, true);
    int available_count = standby_slot_assigned_med_count(slot_idx, false);
    if (assigned_count <= 0) return;

    if (available_count <= 0) {
        safe_copy(buf, buf_len, (lang == UI_LANG_TH) ? "ยาหมด" : "No stock");
        if (color) *color = THEME_BAD;
        return;
    }

    if (dispenser_is_waiting() && dispenser_waiting_slot() == slot_idx) {
        safe_copy(buf, buf_len, (lang == UI_LANG_TH) ? "รอรับยา" : "Waiting");
        if (color) *color = THEME_WARN;
        return;
    }

    const netpie_shadow_t *sh = netpie_get_shadow();
    int slot_minutes = sh ? standby_time_to_minutes(sh->slot_time[slot_idx]) : -1;
    if (slot_minutes < 0 || now_minutes < 0) {
        safe_copy(buf, buf_len, (lang == UI_LANG_TH) ? "ยังไม่ถึงเวลา" : "Upcoming");
        if (color) *color = THEME_OK;
        return;
    }

    if (slot_minutes < now_minutes) {
        if (dispenser_get_missed_slots() & (1 << slot_idx)) {
            safe_copy(buf, buf_len, (lang == UI_LANG_TH) ? "พลาดการทาน" : "Missed");
            if (color) *color = THEME_BAD;
        } else {
            safe_copy(buf, buf_len, (lang == UI_LANG_TH) ? "ผ่านเวลา" : "Passed");
            if (color) *color = THEME_TXT_MUTED;
        }
    } else if (slot_minutes == now_minutes) {
        safe_copy(buf, buf_len, (lang == UI_LANG_TH) ? "ถึงเวลา" : "Due now");
        if (color) *color = THEME_WARN;
    } else {
        safe_copy(buf, buf_len, (lang == UI_LANG_TH) ? "ยังไม่ถึงเวลา" : "Upcoming");
        if (color) *color = THEME_OK;
    }
}

static int standby_build_slot_detail_lines(int slot_idx, char lines[][96], int max_lines, ui_language_t lang)
{
    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh || !sh->loaded || !sh->enabled || !lines || max_lines <= 0) return 0;
    if (slot_idx < 0 || slot_idx >= 7) return 0;

    int line_count = 0;
    for (int i = 0; i < DISPENSER_MED_COUNT && line_count < max_lines; ++i) {
        if (!((sh->med[i].slots >> slot_idx) & 0x01)) continue;

        const char *name = sh->med[i].name[0] ? sh->med[i].name : ((lang == UI_LANG_TH) ? "ไม่ได้ตั้งชื่อยา" : "Unnamed med");
        if (sh->med[i].count > 0) {
            snprintf(lines[line_count], 96, "%d. %s", line_count + 1, name);
        } else {
            snprintf(lines[line_count], 96, "%d. %s %s", line_count + 1, name,
                     (lang == UI_LANG_TH) ? "(หมด)" : "(Out)");
        }
        ++line_count;
    }

    return line_count;
}

static int standby_build_today_schedule_lines(char lines[][128], int max_lines, ui_language_t lang)
{
    if (!lines || max_lines <= 0) return 0;

    int slots[7] = {0};
    int count = standby_collect_today_schedule_slots(slots, max_lines > 7 ? 7 : max_lines);
    const netpie_shadow_t *sh = netpie_get_shadow();

    for (int i = 0; i < count; ++i) {
        char status[32] = "";
        standby_build_slot_status(slots[i], -1, lang, status, sizeof(status), NULL);
        snprintf(lines[i], 128, "%s %s %s",
                 sh ? sh->slot_time[slots[i]] : "--:--",
                 standby_slot_label_popup(slots[i], lang),
                 status);
    }
    return count;
}

static bool standby_date_is_valid(const char *date_str)
{
    if (!date_str || !date_str[0]) return false;

    char day[8] = "";
    int dd = 0, mm = 0, yyyy = 0;
    if (sscanf(date_str, "%7s %d/%d/%d", day, &dd, &mm, &yyyy) != 4) return false;
    if (strcmp(day, "---") == 0 || strcmp(day, "--") == 0) return false;
    if (dd < 1 || dd > 31 || mm < 1 || mm > 12 || yyyy < 2001) return false;
    return true;
}

static const char *standby_translate_weekday_th(const char *day)
{
    if (!day) return "";
    if (strcmp(day, "Sun") == 0) return "อา.";
    if (strcmp(day, "Mon") == 0) return "จ.";
    if (strcmp(day, "Tue") == 0) return "อ.";
    if (strcmp(day, "Wed") == 0) return "พ.";
    if (strcmp(day, "Thu") == 0) return "พฤ.";
    if (strcmp(day, "Fri") == 0) return "ศ.";
    if (strcmp(day, "Sat") == 0) return "ส.";
    return day;
}

static void standby_format_date_for_ui(const char *src, char *dst, size_t dst_len, ui_language_t lang)
{
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!standby_date_is_valid(src)) return;

    char day[8] = "";
    int dd = 0, mm = 0, yyyy = 0;
    if (sscanf(src, "%7s %d/%d/%d", day, &dd, &mm, &yyyy) != 4) return;

    if (lang == UI_LANG_TH) {
        snprintf(dst, dst_len, "%s %02d/%02d/%04d", standby_translate_weekday_th(day), dd, mm, yyyy);
    } else {
        snprintf(dst, dst_len, "%s %02d/%02d/%04d", day, dd, mm, yyyy);
    }
}

void ui_utf8_draw_glyph_mask_scaled(int16_t x, int16_t y, const ui_utf8_font_glyph_t *glyph,
                                           uint16_t color, uint8_t target_height)
{
    if (!glyph || !glyph->bitmap || glyph->width == 0 || glyph->height == 0 || target_height == 0) return;

    uint32_t bit_index = 0;
    for (uint8_t row = 0; row < glyph->height; ++row) {
        int16_t sy0 = (int16_t)((row * target_height) / kUiUtf8FontLineHeight);
        int16_t sy1 = (int16_t)((((int)row + 1) * target_height) / kUiUtf8FontLineHeight);
        int16_t sh = sy1 - sy0;
        if (sh <= 0) sh = 1;

        for (uint8_t col = 0; col < glyph->width; ++col, ++bit_index) {
            uint8_t byte = glyph->bitmap[bit_index >> 3];
            bool on = ((byte >> (7 - (bit_index & 7))) & 0x01u) != 0;
            if (!on) continue;

            int16_t sx0 = (int16_t)((col * target_height) / kUiUtf8FontLineHeight);
            int16_t sx1 = (int16_t)((((int)col + 1) * target_height) / kUiUtf8FontLineHeight);
            int16_t sw = sx1 - sx0;
            if (sw <= 0) sw = 1;
            fill_rect(x + sx0, y + sy0, sw, sh, color);
        }
    }
}

int16_t ui_utf8_text_width_scaled_px(const char *text, uint8_t target_height)
{
    int32_t base_width = ui_utf8_text_width(text);
    return (int16_t)((base_width * target_height) / kUiUtf8FontLineHeight);
}

int16_t ui_utf8_draw_text_scaled_px(int16_t x, int16_t y, const char *text, uint16_t color, uint8_t target_height)
{
    int16_t cursor_x = x;
    int16_t last_base_x = x;
    uint32_t prev_cp = 0;
    const char *cursor = text;

    while (cursor && *cursor) {
        uint32_t cp = ui_utf8_decode_next(&cursor);
        const ui_utf8_font_glyph_t *glyph = ui_utf8_find_glyph(cp);
        if (!glyph) glyph = ui_utf8_find_glyph('?');
        if (!glyph) continue;

        int16_t scaled_x_offset = (int16_t)((glyph->x_offset * target_height) / kUiUtf8FontLineHeight);
        int16_t scaled_y_offset = (int16_t)((glyph->y_offset * target_height) / kUiUtf8FontLineHeight);
        int16_t scaled_advance = (int16_t)((glyph->advance * target_height) / kUiUtf8FontLineHeight);
        if (scaled_advance <= 0 && glyph->advance > 0) scaled_advance = 1;

        if (ui_utf8_is_sara_am(cp)) {
            const ui_utf8_font_glyph_t *nikhahit = ui_utf8_find_glyph(0x0E4D);
            const ui_utf8_font_glyph_t *sara_aa = ui_utf8_find_glyph(0x0E32);

            if (nikhahit) {
                int16_t mark_x = last_base_x + (int16_t)((nikhahit->x_offset * target_height) / kUiUtf8FontLineHeight);
                int16_t mark_y = y + (int16_t)((nikhahit->y_offset * target_height) / kUiUtf8FontLineHeight);
                ui_utf8_draw_glyph_mask_scaled(mark_x, mark_y, nikhahit, color, target_height);
            }

            if (sara_aa) {
                int16_t aa_x = cursor_x + (int16_t)((sara_aa->x_offset * target_height) / kUiUtf8FontLineHeight);
                int16_t aa_y = y + (int16_t)((sara_aa->y_offset * target_height) / kUiUtf8FontLineHeight);
                int16_t aa_advance = (int16_t)((sara_aa->advance * target_height) / kUiUtf8FontLineHeight);
                if (aa_advance <= 0 && sara_aa->advance > 0) aa_advance = 1;
                ui_utf8_draw_glyph_mask_scaled(aa_x, aa_y, sara_aa, color, target_height);
                cursor_x += aa_advance;
                last_base_x = cursor_x - aa_advance;
                continue;
            }
        }

        int16_t draw_x = cursor_x + scaled_x_offset;
        int16_t draw_y = y + scaled_y_offset;

        if (ui_utf8_is_above_mark(cp) || ui_utf8_is_below_mark(cp)) {
            draw_x = last_base_x + scaled_x_offset;
            if (ui_utf8_is_tone_mark(cp) && (ui_utf8_is_upper_vowel(prev_cp) || prev_cp == 0x0E31)) {
                int16_t tone_lift = (int16_t)((6 * target_height) / kUiUtf8FontLineHeight);
                if (tone_lift <= 0) tone_lift = 1;
                int16_t tone_shift = (int16_t)(target_height / 12);
                if (tone_shift <= 0) tone_shift = 1;
                draw_y -= tone_lift;
                draw_x += tone_shift;
            }
        } else {
            last_base_x = cursor_x;
        }

        ui_utf8_draw_glyph_mask_scaled(draw_x, draw_y, glyph, color, target_height);

        if (!(ui_utf8_is_above_mark(cp) || ui_utf8_is_below_mark(cp))) {
            cursor_x += scaled_advance;
        }

        prev_cp = cp;
    }

    return cursor_x;
}

void draw_utf8_centered_line_scaled(int16_t center_x, int16_t top_y, const char *text,
                                           uint16_t fg, uint16_t bg, uint8_t target_height)
{
    int16_t text_w = ui_utf8_text_width_scaled_px(text, target_height);
    int16_t text_x = center_x - (text_w / 2);
    fill_rect(text_x, top_y, text_w, target_height, bg);
    ui_utf8_draw_text_scaled_px(text_x, top_y, text, fg, target_height);
}



static int16_t standby_schedule_text_width_px(const char *text, uint8_t th_height)
{
    if (!text || !text[0]) return 0;
    if (ui_utf8_has_non_ascii(text)) {
        return ui_utf8_text_width_scaled_px(text, th_height);
    }

    const GFXfont *font = (th_height >= 20) ? &FreeSans12pt7b : &FreeSans9pt7b;
    return gfx_text_width(text, font);
}

static void standby_fit_schedule_text(char *dst, size_t dst_cap, const char *src, int16_t max_w, uint8_t th_height)
{
    const char *suffix = "...";
    bool truncated = false;

    if (!dst || dst_cap == 0) return;
    ui_utf8_safe_truncate_copy(dst, dst_cap, src ? src : "");
    if (!dst[0]) return;
    if (standby_schedule_text_width_px(dst, th_height) <= max_w) return;

    while (dst[0]) {
        if (ui_utf8_has_non_ascii(dst)) {
            if (!ui_utf8_backspace(dst)) break;
        } else {
            size_t len = strlen(dst);
            if (len == 0) break;
            dst[len - 1] = '\0';
        }

        if (!dst[0]) break;

        char candidate[96];
        ui_utf8_safe_truncate_copy(candidate, sizeof(candidate), dst);
        if ((strlen(candidate) + strlen(suffix)) < sizeof(candidate)) {
            strcat(candidate, suffix);
        }
        if (standby_schedule_text_width_px(candidate, th_height) <= max_w) {
            ui_utf8_safe_truncate_copy(dst, dst_cap, candidate);
            truncated = true;
            break;
        }
    }

    if (!truncated) {
        dst[0] = '\0';
    }
}

static void draw_schedule_text_line(int16_t x, int16_t top_y, int16_t width, const char *text,
                                    uint16_t fg, uint16_t bg, uint8_t th_height)
{
    char fitted[96];

    fill_rect(x, top_y, width, th_height + 4, bg);
    standby_fit_schedule_text(fitted, sizeof(fitted), text, width, th_height);
    if (!fitted[0]) return;

    if (ui_utf8_has_non_ascii(fitted)) {
        ui_utf8_draw_text_scaled_px(x, top_y, fitted, fg, th_height);
    } else {
        const GFXfont *font = (th_height >= 20) ? &FreeSans12pt7b : &FreeSans9pt7b;
        int16_t baseline_y = top_y + ((th_height >= 20) ? (th_height + 1) : (th_height - 1));
        draw_string_gfx(x, baseline_y, fitted, fg, bg, font);
    }
}

static void draw_schedule_summary_row(int16_t x, int16_t y, int16_t w, int16_t h, int slot_idx,
                                      int now_minutes, ui_language_t lang)
{
    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh) return;

    uint16_t row_bg = ((slot_idx & 1) == 0) ? ST_RGB565(48, 84, 129) : ST_RGB565(44, 79, 122);
    fill_round_rect(x, y, w, h, 8, row_bg);

    char status[32] = "";
    uint16_t status_color = THEME_TXT_MAIN;
    standby_build_slot_status(slot_idx, now_minutes, lang, status, sizeof(status), &status_color);
    uint8_t meal_height = (lang == UI_LANG_EN) ? 16 : 18;
    uint8_t status_height = (lang == UI_LANG_EN) ? 16 : 16;

    draw_string_gfx(x + 10, y + 21, sh->slot_time[slot_idx], 0xFFFF, row_bg, &FreeSans12pt7b);
    draw_schedule_text_line(x + 92, y + 4, 154, standby_slot_label_popup(slot_idx, lang), 0xFFFF, row_bg, meal_height);
    draw_schedule_text_line(x + 262, y + 4, 122, status, status_color, row_bg, status_height);
}

static void draw_schedule_detail_row(int16_t x, int16_t y, int16_t w, int16_t h, const char *text, ui_language_t lang)
{
    uint16_t row_bg = ST_RGB565(46, 81, 125);
    fill_round_rect(x, y, w, h, 8, row_bg);
    draw_schedule_text_line(x + 10, y + 4, w - 20, text, 0xFFFF, row_bg, (lang == UI_LANG_TH) ? 22 : 20);
}

static void draw_alert_detail_row(int16_t x, int16_t y, int16_t w, int16_t h, const char *text,
                                  uint16_t row_bg, ui_language_t lang)
{
    fill_round_rect(x, y, w, h, 8, row_bg);
    draw_schedule_text_line(x + 10, y + 4, w - 20, text, 0xFFFF, row_bg, (lang == UI_LANG_TH) ? 20 : 18);
}

static void draw_schedule_popup_line(int16_t x, int16_t top_y, const char *text, uint16_t fg, uint16_t bg)
{
    draw_schedule_text_line(x, top_y, 360, text, fg, bg, 18);
}

static void draw_standby_page(bool force, const char *hhmm, const char *ss, const char *date_str, const char *dose)
{
    static char prev_hhmm[8] = "";
    static char prev_ss[4] = "";
    static char prev_date[32] = "";
    static char prev_dose[48] = "";

    const uint16_t ST_TIME_CARD    = ST_RGB565(38, 71, 110);
    const uint16_t ST_DOSE_CARD    = ST_RGB565(221, 245, 229);
    const uint16_t ST_DOSE_BORDER  = ST_RGB565(134, 239, 172);
    const uint16_t ST_TXT_TIME     = THEME_TXT_MAIN;
    const uint16_t ST_TXT_DATE     = THEME_TXT_MUTED;
    const uint16_t ST_DOSE_MUTED   = ST_RGB565(78, 124, 96);
    const uint16_t ST_UP_TITLE     = ST_RGB565(34, 94, 58);
    const uint16_t ST_UP_TEXT      = ST_RGB565(22, 101, 52);
    const uint16_t ST_ONLINE       = ST_RGB565(34, 197, 94);
    const uint16_t ST_OFFLINE      = ST_RGB565(150, 150, 150);
    const uint16_t ST_SYNC_OK      = ST_RGB565(14, 116, 144);
    const uint16_t ST_SYNC_WAIT    = ST_RGB565(217, 119, 6);
    const uint16_t ST_SYNC_IDLE    = ST_RGB565(100, 116, 139);
    const uint16_t ST_TXT_IP       = ST_RGB565(132, 167, 198);
    const uint16_t ST_BG_TOP       = ST_RGB565(21, 43, 71);
    const uint16_t ST_BG_BOT       = ST_RGB565(31, 58, 92);

    const int TIME_X = 16;
    const int TIME_Y = 16;
    const int TIME_W = LCD_W - 32;
    const int TIME_H = 148;

    const int DOSE_X = 16;
    const int DOSE_Y = 174;
    const int DOSE_W = LCD_W - 32;
    const int DOSE_H = 82;

    const int FOOT_X = 16;
    const int FOOT_Y = 258;
    const int FOOT_W = LCD_W - 32;
    const int FOOT_H = 46;

    // พิกัดยึดกึ่งกลางหน้าจอ
    const int TIME_BLOCK_Y = TIME_Y + 14;
    const int TIME_BLOCK_H = 84;
    const int TIME_BASELINE_Y = 88;

    const int DATE_X = 68;
    const int DATE_Y = 112;
    const int DATE_W = 344;
    const int DATE_H = 36;
    const int DATE_BASELINE_Y = 140;

    if (force) {
        draw_gradient_v(0, 0, LCD_W, LCD_H, ST_BG_TOP, ST_BG_BOT);

        fill_round_rect_frame(TIME_X, TIME_Y, TIME_W, TIME_H, 14, ST_TIME_CARD, THEME_BORDER);
        fill_round_rect_frame(DOSE_X, DOSE_Y, DOSE_W, DOSE_H, 14, ST_DOSE_CARD, ST_DOSE_BORDER);
        fill_round_rect_frame(FOOT_X, FOOT_Y, FOOT_W, FOOT_H, 12, ST_TIME_CARD, THEME_BORDER);

        s_ip_dirty = true;
        prev_hhmm[0] = '\0';
        prev_ss[0] = '\0';
        prev_date[0] = '\0';
        prev_dose[0] = '\0';
    }

    const int time_region_x = TIME_X + 16;
    const int time_region_w = TIME_W - 32;
    const int TIME_SCALE = 2;
    const int16_t hhmm_area_w = gfx_text_width_scaled("88:88", &FreeSansBold24pt7b, TIME_SCALE);
    const int16_t ss_area_w = gfx_text_width_scaled(":88", &FreeSansBold24pt7b, TIME_SCALE);
    const int16_t total_w = hhmm_area_w + ss_area_w;
    const int16_t left_x = time_region_x + ((time_region_w - total_w) / 2);
    const int16_t ss_x = left_x + hhmm_area_w;

    if (force || strcmp(hhmm, prev_hhmm) != 0) {
        safe_copy(prev_hhmm, sizeof(prev_hhmm), hhmm);
        fill_rect(left_x, TIME_BLOCK_Y, hhmm_area_w, TIME_BLOCK_H, ST_TIME_CARD);
        draw_string_gfx_scaled(left_x, TIME_BASELINE_Y, hhmm, ST_TXT_TIME, ST_TIME_CARD, &FreeSansBold24pt7b, TIME_SCALE);
    }

    if (force || strcmp(ss, prev_ss) != 0) {
        safe_copy(prev_ss, sizeof(prev_ss), ss);

        char ss_with_colon[8];
        snprintf(ss_with_colon, sizeof(ss_with_colon), ":%s", ss);

        // No fill_rect here — draw_string_gfx_scaled paints background
        // pixels in the glyph bounding box itself (fg/bg pair), so the
        // explicit pre-clear was redundant and the brief blank window
        // between fill and draw was the visible per-second flicker on
        // the standby clock seconds.
        draw_string_gfx_scaled(ss_x, TIME_BASELINE_Y, ss_with_colon, ST_TXT_TIME, ST_TIME_CARD, &FreeSansBold24pt7b, TIME_SCALE);
    }

    // --- redraw date เฉพาะตอนเปลี่ยน ---
    if (force || strcmp(date_str, prev_date) != 0) {
        safe_copy(prev_date, sizeof(prev_date), date_str);
        fill_rect(DATE_X, DATE_Y, DATE_W, DATE_H, ST_TIME_CARD);
        if (g_ui_language == UI_LANG_TH) {
            draw_utf8_centered_line_scaled(LCD_W / 2, DATE_Y + 8, date_str, ST_TXT_DATE, ST_TIME_CARD, 22);
        } else {
            draw_string_centered(LCD_W / 2, DATE_BASELINE_Y, date_str, ST_TXT_DATE, ST_TIME_CARD, &FreeSans18pt7b);
        }
    }

    if (force || strcmp(dose, prev_dose) != 0) {
        safe_copy(prev_dose, sizeof(prev_dose), dose);

        const int content_left = DOSE_X + 18;
        const int content_right = DOSE_X + DOSE_W - 18;
        const int content_center_x = content_left + ((content_right - content_left) / 2);

        fill_round_rect(DOSE_X + 4, DOSE_Y + 4, DOSE_W - 8, DOSE_H - 8, 10, ST_DOSE_CARD);

        if (strcmp(dose, "No schedule") == 0 || strlen(dose) == 0) {
            if (g_ui_language == UI_LANG_TH) {
                // User asked for the "ยังไม่มีตารางยาถัดไป" line to be larger.
                // The 158x16 bitmap label was too small; switch to the
                // scaled UTF-8 renderer at the same size used elsewhere
                // for prominent Thai status text.
                draw_utf8_centered_line_scaled(content_center_x, DOSE_Y + 28,
                                               "ยังไม่มีตารางยาถัดไป",
                                               ST_DOSE_MUTED, ST_DOSE_CARD, 28);
            } else {
                draw_string_centered(content_center_x, DOSE_Y + 52, "No upcoming schedule", ST_DOSE_MUTED, ST_DOSE_CARD, &FreeSans12pt7b);
            }
        } else {
            if (g_ui_language == UI_LANG_TH) {
                char line[64] = "";
                if (standby_build_next_dose_th_single_line(dose, line, sizeof(line))) {
                    draw_utf8_centered_line_scaled(content_center_x, DOSE_Y + 24, line, ST_UP_TEXT, ST_DOSE_CARD, 36);
                }
            } else {
                char slot[40] = "";
                char time[16] = "";
                char phase[16] = "";
                char meal[24] = "";
                if (standby_build_next_dose_en(dose, slot, sizeof(slot), time, sizeof(time))) {
                    standby_split_dose_slot_en(slot, phase, sizeof(phase), meal, sizeof(meal));
                    draw_standby_next_dose_en_line(content_left, content_right, DOSE_Y + 54,
                                                   phase, meal, time,
                                                   ST_UP_TITLE, ST_UP_TEXT, ST_DOSE_CARD);
                }
            }
        }
    }

    static char prev_ip_line[40] = "";
    static char prev_sync_line[24] = "";
    char ip_line[40] = "";
    char sync_line[24] = "";
    char wifi_name[40] = "";
    char web_url[64] = "";
    bool wifi_online = wifi_sta_connected() && strcmp(s_ip, "0.0.0.0") != 0;
    bool ap_mode = (!wifi_online && strcmp(s_ip, "192.168.4.1") == 0);
    size_t event_pending_count = offline_sync_pending_event_count();
    size_t telegram_pending_count = offline_sync_pending_telegram_count();
    size_t gsheet_pending_count = offline_sync_pending_gsheet_count();
    size_t shadow_pending_count = offline_sync_pending_shadow_count();
    uint16_t sync_bg = ST_SYNC_IDLE;

    if (wifi_online) {
        snprintf(ip_line, sizeof(ip_line), "IP: %s", s_ip);
    } else if (ap_mode) {
        safe_copy(ip_line, sizeof(ip_line), "AP: 192.168.4.1");
    } else {
        safe_copy(wifi_name, sizeof(wifi_name), (g_ui_language == UI_LANG_TH) ? "ยังไม่เชื่อม Wi-Fi" : "Wi-Fi not connected");
        safe_copy(web_url, sizeof(web_url), (g_ui_language == UI_LANG_TH) ? "แตะเพื่อดูการเชื่อมต่อ" : "Tap to view network");
    }

    if (ip_line[0] == '\0') {
        safe_copy(ip_line, sizeof(ip_line), "IP: 0.0.0.0");
    }

    if (gsheet_pending_count > 0) {
        snprintf(sync_line, sizeof(sync_line), "GS %u", (unsigned)gsheet_pending_count);
        sync_bg = ST_SYNC_WAIT;
    } else if (telegram_pending_count > 0) {
        snprintf(sync_line, sizeof(sync_line), "TG %u", (unsigned)telegram_pending_count);
        sync_bg = ST_SYNC_WAIT;
    } else if (shadow_pending_count > 0) {
        snprintf(sync_line, sizeof(sync_line), "NETPIE %u", (unsigned)shadow_pending_count);
        sync_bg = ST_SYNC_IDLE;
    } else if (event_pending_count > 0) {
        snprintf(sync_line, sizeof(sync_line), "EVENT %u", (unsigned)event_pending_count);
        sync_bg = ST_SYNC_WAIT;
    } else if (wifi_online) {
        safe_copy(sync_line, sizeof(sync_line), "SYNC OK");
        sync_bg = ST_SYNC_OK;
    } else {
        safe_copy(sync_line, sizeof(sync_line), "NO PENDING");
    }

    if (force || s_ip_dirty || strcmp(ip_line, prev_ip_line) != 0 || strcmp(sync_line, prev_sync_line) != 0) {
        s_ip_dirty = false;
        safe_copy(prev_ip_line, sizeof(prev_ip_line), ip_line);
        safe_copy(prev_sync_line, sizeof(prev_sync_line), sync_line);
        fill_rect(FOOT_X + 10, FOOT_Y + 4, FOOT_W - 20, FOOT_H - 8, ST_TIME_CARD);

        if (!wifi_online) {
            fill_round_rect(FOOT_X + 10, FOOT_Y + 7, 84, 22, 8, ST_OFFLINE);
            if (g_ui_language == UI_LANG_TH) {
                draw_standby_label(FOOT_X + 10 + ((84 - kThOffline.width) / 2), FOOT_Y + 12, &kThOffline);
            } else {
                draw_string_centered(FOOT_X + 52, FOOT_Y + 23, "OFFLINE", 0xFFFF, ST_OFFLINE, &FreeSans9pt7b);
            }
        } else {
            fill_round_rect(FOOT_X + 10, FOOT_Y + 7, 84, 22, 8, ST_ONLINE);
            if (g_ui_language == UI_LANG_TH) {
                draw_standby_label(FOOT_X + 10 + ((84 - kThOnline.width) / 2), FOOT_Y + 12, &kThOnline);
            } else {
                draw_string_centered(FOOT_X + 52, FOOT_Y + 23, "ONLINE", 0xFFFF, ST_ONLINE, &FreeSans9pt7b);
            }
        }

        draw_string_gfx(FOOT_X + 112, FOOT_Y + 26, ip_line, ST_TXT_IP, ST_TIME_CARD, &FreeSans12pt7b);
    }
}

static void ui_standby_render_modal(uint32_t now)
{
    uint32_t now_ms = now * portTICK_PERIOD_MS;

    bool is_forced = force_redraw;
    if (force_redraw) force_redraw = false;

    // Run hardware health checks ONLY on the 10-second timer, NOT on
    // every force_redraw. Force-redraw fires whenever UI state changes
    // (touch on a popup button, page switch, etc.); calling ping_3x ×
    // 3 devices on every redraw makes the UI freeze 1-3 s when the bus
    // is in a bad state, because each ping_3x can block on i2c master
    // mutex + 3 retries with delays. The cached verdicts from the last
    // timer check are good enough for a redraw — modal will catch up
    // on the next scheduled health window.
    if (now_ms >= s_hw_health_next_check_ms) {
        // Hysteresis: only flip "down" after CONSECUTIVE_FAIL_THRESHOLD
        // failed health windows in a row (default 2 = 20 s). Cuts modal
        // false-alarms when bus glitches briefly mid-operation.
        // ESP_ERR_INVALID_STATE means the i2c driver itself is wedged —
        // keep poking it and we trigger a store fault inside ISR. Skip
        // remaining pings AND don't update fail counts on a wedge so a
        // transient driver hiccup doesn't escalate to a "device down"
        // verdict on its own.
        constexpr int CONSECUTIVE_FAIL_THRESHOLD = 2;
        static int s_fail_pca = 0, s_fail_pcf = 0, s_fail_rtc = 0;
        // Track whether each device has *ever* answered OK in this boot.
        // We only raise the modal for devices that were once known
        // present and then went away — never-seen devices are treated as
        // "not configured" so a flaky probe at boot doesn't permanently
        // alarm the user even when their wiring is correct and the next
        // round will succeed. This is the fix for the "boot probe NACK
        // → modal stays even after bus recovers" pattern.
        static bool s_seen_pca = false, s_seen_pcf = false, s_seen_rtc = false;

        auto ping_3x = [](uint16_t addr) -> esp_err_t {
            for (int a = 0; a < 3; ++a) {
                esp_err_t r = i2c_manager_ping(addr);
                if (r == ESP_OK) return ESP_OK;
                if (r == ESP_ERR_INVALID_STATE) return r;
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            return ESP_FAIL;
        };
        auto update_one = [&](uint16_t addr, int *fail_count, bool *cache, bool *seen) -> bool {
            esp_err_t r = ping_3x(addr);
            if (r == ESP_ERR_INVALID_STATE) {
                return false;  // bus wedged — leave verdict alone
            }
            if (r == ESP_OK) {
                *seen = true;
                *fail_count = 0;
                *cache = false;
                return true;
            }
            // NACK / no response. Only flag if we've ever seen this
            // device alive. If we've never seen it (boot probe missed,
            // user may be running without that module), keep quiet.
            if (!*seen) { *cache = false; return true; }
            if (*fail_count < CONSECUTIVE_FAIL_THRESHOLD) (*fail_count)++;
            if (*fail_count >= CONSECUTIVE_FAIL_THRESHOLD) *cache = true;
            return true;
        };
        if (update_one(ADDR_PCA9685, &s_fail_pca, &s_cached_pca_err, &s_seen_pca) &&
            update_one(ADDR_PCF8574, &s_fail_pcf, &s_cached_pcf_err, &s_seen_pcf)) {
            update_one(ADDR_DS3231, &s_fail_rtc, &s_cached_rtc_err, &s_seen_rtc);
        }
        s_hw_health_next_check_ms = now_ms + 10000;
    }

    if (s_netpie_sync_popup_until > 0 && now_ms >= s_netpie_sync_popup_until) {
        s_netpie_sync_popup_until = 0;
        if (s_popup_state == 3) {
            s_popup_state = 0;
            is_forced = true;
        }
    }

    char t_str[16] = "--:--:--";
    if (!s_cached_rtc_err) {
        ds3231_get_time_str(t_str, sizeof(t_str));
    }

    char hhmm[6] = "--:--";
    char ss[3] = "--";
    if (strlen(t_str) >= 8) {
        strncpy(hhmm, t_str, 5);
        hhmm[5] = '\0';
        strncpy(ss, t_str + 6, 2);
        ss[2] = '\0';
    }

    static char last_valid_date_raw[32] = "";
    char date_raw[32] = "";
    char date_str[32] = "-- --/--/----";
    if (!s_cached_rtc_err) {
        if (ds3231_get_date_str(date_raw, sizeof(date_raw)) == ESP_OK && standby_date_is_valid(date_raw)) {
            safe_copy(last_valid_date_raw, sizeof(last_valid_date_raw), date_raw);
        }
    }
    if (last_valid_date_raw[0] != '\0') {
        standby_format_date_for_ui(last_valid_date_raw, date_str, sizeof(date_str), g_ui_language);
    }

    char dose_str[48] = "No schedule";
    dispenser_get_next_dose_str(dose_str, sizeof(dose_str));

    const netpie_shadow_t *sh = netpie_get_shadow();
    bool schedule_ok = false;
    if (!sh->loaded || sh->enabled == false) {
        schedule_ok = true;
    } else {
        for (int i = 0; i < 7; ++i) {
            if (sh->slot_time[i][0] != '\0') {
                schedule_ok = true;
                break;
            }
        }
    }

    bool pca_err = s_cached_pca_err;
    bool pcf_err = s_cached_pcf_err;
    bool rtc_err = s_cached_rtc_err;
    bool hw_err = (pca_err || pcf_err || rtc_err);
    uint8_t hw_mask = 0;
    if (pca_err) hw_mask |= 0x01;
    if (pcf_err) hw_mask |= 0x02;
    if (rtc_err) hw_mask |= 0x04;

    if (!hw_err && s_popup_state == 2) {
        // Bus came back. Tear down the modal but DON'T reset
        // s_hw_warn_dismissed *or* s_hw_alert_mask — a flapping bus
        // oscillates "all good ⇄ all down" every 10 s; if we zeroed the
        // mask here, the next failure with the same device set would
        // satisfy the "(hw_mask & ~s_hw_alert_mask) != 0" new-bit check
        // and re-arm dismiss, popping the modal again. Keeping the mask
        // means: dismissed once → stays dismissed for the same fault
        // pattern, even across recovery flaps.
        s_hw_alert_drawn = false;
        s_today_schedule_popup_drawn = false;
        s_popup_state = 0;
        is_forced = true;
    }

    if (s_popup_state != 1 && s_popup_state != 2 && s_popup_state != 3 &&
        s_popup_state != 4 && s_popup_state != 5) {
        draw_standby_page(is_forced, hhmm, ss, date_str, dose_str);
    }

    if (hw_err) {
        // Once dismissed, stay dismissed *until a new device fails*.
        // The bus flaps every health-check window (devices appearing /
        // disappearing every 10 s); without sticky dismiss the modal
        // would pop again on every cycle. Only the *new bits* in the
        // failing mask compared to what was failing at dismiss time
        // count as a fresh fault.
        if (s_hw_warn_dismissed) {
            if ((hw_mask & ~s_hw_alert_mask) == 0) {
                // No new device joined the failing set — keep silenced.
                return;
            }
            // New failure → re-arm.
            s_hw_warn_dismissed = false;
        }
        if (s_popup_state == 2 && s_hw_alert_drawn && !is_forced && hw_mask == s_hw_alert_mask) {
            return;
        }

        fill_round_rect_frame(40, 50, 400, 220, 15, THEME_BAD, 0xFFFF);
        if (g_ui_language == UI_LANG_TH) {
            draw_standby_label((LCD_W - kThHwError1.width) / 2, 68, &kThHwError1);
        } else {
            draw_string_centered(240, 85, "HARDWARE ERROR!", 0xFFFF, THEME_BAD, &FreeSansBold18pt7b);
        }

        const uint16_t alert_row_bg = ST_RGB565(164, 34, 52);
        int row_y = 108;
        if (pca_err) {
            draw_alert_detail_row(68, row_y, 304, 24,
                                  (g_ui_language == UI_LANG_TH) ? "Servo Driver [PCA9685]" : "Servo Driver [PCA9685]",
                                  alert_row_bg, g_ui_language);
            row_y += 28;
        }
        if (pcf_err) {
            draw_alert_detail_row(68, row_y, 304, 24,
                                  (g_ui_language == UI_LANG_TH) ? "Sensor Expander [PCF8574]" : "Sensor Expander [PCF8574]",
                                  alert_row_bg, g_ui_language);
            row_y += 28;
        }
        if (rtc_err) {
            draw_alert_detail_row(68, row_y, 304, 24,
                                  (g_ui_language == UI_LANG_TH) ? "RTC Clock [DS3231]" : "RTC Clock [DS3231]",
                                  alert_row_bg, g_ui_language);
            row_y += 28;
        }

        // Bus is stuck: only a full power-cycle of the modules' VCC fixes
        // it (chip reset alone leaves slaves in their hung state). Tell
        // the user that directly — the previous "check wiring" hint just
        // wasted time when the wiring is actually fine.
        draw_schedule_text_line(58, 188, 320,
                                "Unplug USB power for 5 sec to recover",
                                0xFFFF, THEME_BAD, 16);

        // Dismiss button — lets the user keep using the device when a
        // module is still listed as missing but everything else works.
        // The modal will reappear automatically if the failing-device
        // set changes (covered by the hw_mask compare above).
        draw_standby_modal_button(kAlertButtonX, kAlertButtonY, kAlertButtonW, kAlertButtonH, ST_RGB565(185, 28, 28), 0xFFFF);

        s_hw_alert_drawn = true;
        s_hw_alert_mask = hw_mask;
        s_popup_state = 2;
        return;
    } else {
        // All hardware reachable again — clear dismiss so a future fault
        // can repaint the modal.
        s_hw_warn_dismissed = false;
    }

    if (s_netpie_sync_popup_until > 0 && now_ms < s_netpie_sync_popup_until) {
        if (s_popup_state != 3 || is_forced) {
            fill_round_rect_frame(60, 100, 360, 120, 15, THEME_OK, 0xFFFF);
            if (g_ui_language == UI_LANG_TH) {
                draw_standby_label((LCD_W - kThSyncOk1.width) / 2, 128, &kThSyncOk1);
                draw_standby_label((LCD_W - kThSyncOk2.width) / 2, 172, &kThSyncOk2);
            } else {
                draw_string_centered(240, 145, "NETPIE SYNC", 0xFFFF, THEME_OK, &FreeSansBold18pt7b);
                draw_string_centered(240, 185, "Schedule Updated Successfully!", 0xFFFF, THEME_OK, &FreeSans12pt7b);
            }
            s_popup_state = 3;
        }
        return;
    }

    if (!schedule_ok) {
        if (!s_sched_warn_dismissed && (s_popup_state != 1 || is_forced)) {
            fill_round_rect_frame(kAlertPopupX, kAlertPopupY, kAlertPopupW, kAlertPopupH, 16, THEME_WARN, 0xFFFF);
            if (g_ui_language == UI_LANG_TH) {
                draw_standby_label_centered(LCD_W / 2, 82, &kThNoSchedule1);
                draw_standby_label_centered(LCD_W / 2, 130, &kThNoSchedule2);
            } else {
                draw_string_centered(LCD_W / 2, 102, "NO SCHEDULE DETECTED", 0xFFFF, THEME_WARN, &FreeSansBold18pt7b);
                draw_string_centered(LCD_W / 2, 146, "Please set schedule via Netpie or Menu", 0xFFFF, THEME_WARN, &FreeSans12pt7b);
            }

            draw_standby_modal_button(kAlertButtonX, kAlertButtonY, kAlertButtonW, kAlertButtonH, ST_RGB565(185, 28, 28), 0xFFFF);

            s_popup_state = 1;
            return;
        }
        // Schedule still empty but the warning has been dismissed.
        // Don't fall back to the bare standby — that hides any other
        // popup the user has explicitly opened (state 4 = today's
        // schedule list, state 5 = slot detail). Without this fix the
        // "ดูตารางยาวันนี้" tap silently no-ops once the user has
        // dismissed the schedule warning even once.
        if (s_popup_state == 1) {
            return;
        }
        // fall through to popup-state handlers below
    }

    if (s_popup_state == 4) {
        if (s_today_schedule_popup_drawn) return;

        ESP_LOGI("popup4", "enter render");
        int now_minutes = standby_time_to_minutes(hhmm);
        ESP_LOGI("popup4", "time_to_min OK now=%d", now_minutes);

        uint8_t missed_mask = dispenser_get_missed_slots();
        int missed_count = 0;
        for (int i = 0; i < 7; i++) {
            if (missed_mask & (1 << i)) missed_count++;
        }
        ESP_LOGI("popup4", "missed_mask OK %d", missed_count);

        int all_slots[7] = {0};
        int count = standby_collect_today_schedule_slots(all_slots, 7);
        ESP_LOGI("popup4", "collect_slots OK count=%d", count);
        s_schedule_visible_count = 0;
        for (int i = 0; i < count; i++) {
            if (s_show_only_missed) {
                if (missed_mask & (1 << all_slots[i])) {
                    s_schedule_visible_slots[s_schedule_visible_count++] = all_slots[i];
                }
            } else {
                s_schedule_visible_slots[s_schedule_visible_count++] = all_slots[i];
            }
        }

        ESP_LOGI("popup4", "frame fill start");
        fill_round_rect_frame(kSchedulePopupX, kSchedulePopupY, kSchedulePopupW, kSchedulePopupH, 16, THEME_PANEL, 0xFFFF);
        ESP_LOGI("popup4", "frame fill done");
        fill_round_rect(kScheduleCloseX, kScheduleCloseY, kScheduleCloseW, kScheduleCloseH, 8, 0xFFFF);
        draw_string_centered(kScheduleCloseX + (kScheduleCloseW / 2), kScheduleCloseY + 23, "X", THEME_PANEL, 0xFFFF, &FreeSans12pt7b);
        ESP_LOGI("popup4", "header done");

        if (g_ui_language == UI_LANG_TH) {
            ESP_LOGI("popup4", "TH render start");
            draw_utf8_centered_line_scaled(LCD_W / 2, 42, s_show_only_missed ? "มื้อที่พลาดไป" : "ตารางยาวันนี้", 0xFFFF, THEME_PANEL, 26);
            ESP_LOGI("popup4", "TH title done");
            
            char toggle_str[64];
            if (s_show_only_missed) {
                snprintf(toggle_str, sizeof(toggle_str), "ดูทั้งหมด");
            } else {
                snprintf(toggle_str, sizeof(toggle_str), "พลาดไป %d มื้อ", missed_count);
            }
            uint16_t btn_color = s_show_only_missed ? THEME_TXT_MUTED : ((missed_count > 0) ? THEME_BAD : ST_RGB565(34, 197, 94));
            draw_schedule_text_line(kSchedulePopupX + 16, 46, 100, toggle_str, btn_color, THEME_PANEL, 16);
            
            draw_schedule_text_line(52, 76, 54, "เวลา", THEME_TXT_MUTED, THEME_PANEL, 20);
            draw_schedule_text_line(132, 76, 96, "มื้อ", THEME_TXT_MUTED, THEME_PANEL, 20);
            draw_schedule_text_line(300, 76, 104, "สถานะ", THEME_TXT_MUTED, THEME_PANEL, 20);
        } else {
            draw_string_centered(240, 60, s_show_only_missed ? "MISSED DOSES" : "TODAY'S SCHEDULE", 0xFFFF, THEME_PANEL, &FreeSans12pt7b);
            
            char toggle_str[64];
            if (s_show_only_missed) {
                snprintf(toggle_str, sizeof(toggle_str), "Show All");
            } else {
                snprintf(toggle_str, sizeof(toggle_str), "Missed: %d", missed_count);
            }
            uint16_t btn_color = s_show_only_missed ? THEME_TXT_MUTED : ((missed_count > 0) ? THEME_BAD : ST_RGB565(34, 197, 94));
            draw_string_gfx(kSchedulePopupX + 16, 60, toggle_str, btn_color, THEME_PANEL, &FreeSans9pt7b);
            
            draw_string_gfx(52, 96, "TIME", THEME_TXT_MUTED, THEME_PANEL, &FreeSans12pt7b);
            draw_string_gfx(132, 96, "MEAL", THEME_TXT_MUTED, THEME_PANEL, &FreeSans12pt7b);
            draw_string_gfx(300, 96, "STATUS", THEME_TXT_MUTED, THEME_PANEL, &FreeSans12pt7b);
        }

        if (s_schedule_visible_count == 0) {
            if (g_ui_language == UI_LANG_TH) {
                draw_utf8_centered_line_scaled(LCD_W / 2, 140, s_show_only_missed ? "ไม่มีมื้อที่พลาด" : "วันนี้ยังไม่มีรายการจ่ายยา", THEME_TXT_MUTED, THEME_PANEL, 22);
            } else {
                draw_string_centered(240, 164, s_show_only_missed ? "No missed doses today" : "No dispensing schedule for today", THEME_TXT_MUTED, THEME_PANEL, &FreeSans12pt7b);
            }
        } else {
            ESP_LOGI("popup4", "drawing %d rows", s_schedule_visible_count);
            for (int i = 0; i < s_schedule_visible_count; ++i) {
                ESP_LOGI("popup4", "row %d slot=%d start", i, s_schedule_visible_slots[i]);
                draw_schedule_summary_row(kScheduleRowX, kScheduleRowY + (i * kScheduleRowH),
                                          kScheduleRowW, kScheduleRowH,
                                          s_schedule_visible_slots[i], now_minutes, g_ui_language);
                ESP_LOGI("popup4", "row %d done", i);
            }
        }

        ESP_LOGI("popup4", "all done");
        s_today_schedule_popup_drawn = true;
        return;
    }

    if (s_popup_state == 5) {
        if (s_today_schedule_popup_drawn) return;

        char detail_lines[DISPENSER_MED_COUNT][96] = {{0}};
        int detail_count = standby_build_slot_detail_lines(s_schedule_detail_slot, detail_lines, DISPENSER_MED_COUNT, g_ui_language);
        const netpie_shadow_t *detail_sh = netpie_get_shadow();

        fill_round_rect_frame(kSchedulePopupX, kSchedulePopupY, kSchedulePopupW, kSchedulePopupH, 16, THEME_PANEL, 0xFFFF);
        fill_round_rect(kScheduleCloseX, kScheduleCloseY, kScheduleCloseW, kScheduleCloseH, 8, 0xFFFF);
        if (g_ui_language == UI_LANG_TH) {
            draw_utf8_centered_line_scaled(kScheduleCloseX + (kScheduleCloseW / 2), kScheduleCloseY + 5, "กลับ", THEME_PANEL, 0xFFFF, 20);
            draw_utf8_centered_line_scaled(LCD_W / 2, 42, "รายละเอียดมื้อนี้", 0xFFFF, THEME_PANEL, 26);
            if (detail_sh && s_schedule_detail_slot >= 0 && s_schedule_detail_slot < 7) {
                char subtitle[64];
                snprintf(subtitle, sizeof(subtitle), "%s  %s",
                         standby_slot_label_popup(s_schedule_detail_slot, g_ui_language),
                         detail_sh->slot_time[s_schedule_detail_slot]);
                draw_schedule_text_line(64, 78, 340, subtitle, THEME_TXT_MUTED, THEME_PANEL, 22);
            }
        } else {
            draw_string_centered(kScheduleCloseX + (kScheduleCloseW / 2), kScheduleCloseY + 23, "Back", THEME_PANEL, 0xFFFF, &FreeSans12pt7b);
            draw_string_centered(kScheduleHeaderCenterX, 74, "SLOT DETAILS", 0xFFFF, THEME_PANEL, &FreeSansBold18pt7b);
            if (detail_sh && s_schedule_detail_slot >= 0 && s_schedule_detail_slot < 7) {
                char subtitle[64];
                snprintf(subtitle, sizeof(subtitle), "%s  %s",
                         standby_slot_label_popup(s_schedule_detail_slot, g_ui_language),
                         detail_sh->slot_time[s_schedule_detail_slot]);
                draw_schedule_text_line(kScheduleHeaderTextX, 84, kScheduleHeaderTextW, subtitle, THEME_TXT_MUTED, THEME_PANEL, 18);
            }
        }

        if (detail_count == 0) {
            if (g_ui_language == UI_LANG_TH) {
                draw_utf8_centered_line_scaled(LCD_W / 2, 142, "มื้อนี้ยังไม่มียาที่ตั้งไว้", THEME_TXT_MUTED, THEME_PANEL, 22);
            } else {
                draw_string_centered(240, 164, "No medicines assigned to this slot", THEME_TXT_MUTED, THEME_PANEL, &FreeSans12pt7b);
            }
        } else {
            for (int i = 0; i < detail_count; ++i) {
                draw_schedule_detail_row(44, 112 + (i * 28), 392, 26, detail_lines[i], g_ui_language);
            }
        }

        s_today_schedule_popup_drawn = true;
        return;
    }

    s_today_schedule_popup_drawn = false;
    s_schedule_visible_count = 0;
    s_schedule_detail_slot = -1;
}

void ui_standby_render(uint32_t now)
{
    ui_standby_render_modal(now);
    return;
}

static void ui_standby_handle_touch_modal(uint16_t tx_n, uint16_t ty_n)
{
    if (s_popup_state == 1) {
        if (tx_n >= kAlertButtonX && tx_n <= (kAlertButtonX + kAlertButtonW) &&
            ty_n >= kAlertButtonY && ty_n <= (kAlertButtonY + kAlertButtonH)) {
            s_sched_warn_dismissed = true;
            s_today_schedule_popup_drawn = false;
            s_popup_state = 0;
            force_redraw = true;
        }
        return;
    }

    if (s_popup_state == 2) {
        // HW alert modal — dismiss when user taps the button rect. Keep
        // the rest of the popup tap-through-to-clear so an accidental
        // outside-tap doesn't silence a real fault. Once dismissed, the
        // modal stays hidden until a *new* device fails (the dismiss
        // logic compares hw_mask against s_hw_alert_mask).
        if (tx_n >= kAlertButtonX && tx_n <= (kAlertButtonX + kAlertButtonW) &&
            ty_n >= kAlertButtonY && ty_n <= (kAlertButtonY + kAlertButtonH)) {
            s_hw_warn_dismissed = true;
            s_hw_alert_drawn = false;
            s_popup_state = 0;
            force_redraw = true;
        }
        return;
    }

    if (s_popup_state == 4) {
        bool in_popup = (tx_n >= kSchedulePopupX && tx_n <= (kSchedulePopupX + kSchedulePopupW) &&
                         ty_n >= kSchedulePopupY && ty_n <= (kSchedulePopupY + kSchedulePopupH));
        bool on_close = (tx_n >= kScheduleCloseX && tx_n <= (kScheduleCloseX + kScheduleCloseW) &&
                         ty_n >= kScheduleCloseY && ty_n <= (kScheduleCloseY + kScheduleCloseH));
        bool on_toggle = (tx_n >= kSchedulePopupX + 10 && tx_n <= kSchedulePopupX + 130 &&
                          ty_n >= 35 && ty_n <= 65);

        if (on_close || !in_popup) {
            dfplayer_play_track(g_snd_button); // Back sound
            s_show_only_missed = false; // Reset toggle on close
            s_today_schedule_popup_drawn = false;
            s_schedule_visible_count = 0;
            s_schedule_detail_slot = -1;
            s_popup_state = 0;
            force_redraw = true;
            return;
        }

        if (on_toggle) {
            if (!s_show_only_missed) {
                // Tapping "Missed X doses" to show missed
                if (g_ui_language == UI_LANG_TH) dfplayer_play_track(91);
                else dfplayer_play_track(92);
            } else {
                // Tapping "Show All" to show all
                if (g_ui_language == UI_LANG_TH) dfplayer_play_track(93); // Assuming 93
                else dfplayer_play_track(94); // Assuming 94
            }
            s_show_only_missed = !s_show_only_missed;
            s_today_schedule_popup_drawn = false;
            force_redraw = true;
            return;
        }

        for (int i = 0; i < s_schedule_visible_count; ++i) {
            int row_top = kScheduleRowY + (i * kScheduleRowH);
            int row_bottom = row_top + kScheduleRowH;
            if (tx_n >= kScheduleRowX && tx_n <= (kScheduleRowX + kScheduleRowW) &&
                ty_n >= row_top && ty_n <= row_bottom) {
                s_schedule_detail_slot = s_schedule_visible_slots[i];
                s_today_schedule_popup_drawn = false;
                s_popup_state = 5;
                force_redraw = true;
                return;
            }
        }
        return;
    }

    if (s_popup_state == 5) {
        bool in_popup = (tx_n >= kSchedulePopupX && tx_n <= (kSchedulePopupX + kSchedulePopupW) &&
                         ty_n >= kSchedulePopupY && ty_n <= (kSchedulePopupY + kSchedulePopupH));
        bool on_close = (tx_n >= kScheduleCloseX && tx_n <= (kScheduleCloseX + kScheduleCloseW) &&
                         ty_n >= kScheduleCloseY && ty_n <= (kScheduleCloseY + kScheduleCloseH));

        if (on_close || !in_popup) {
            dfplayer_play_track(g_snd_button); // Back sound
            s_today_schedule_popup_drawn = false;
            s_popup_state = 4;
            force_redraw = true;
        }
        return;
    }

    if (tx_n < 150 && ty_n >= 260) {
        if (strcmp(s_ip, "0.0.0.0") != 0) pending_page = PAGE_WIFI_STATUS;
        else pending_page = PAGE_WIFI_SCAN;
    } else if (tx_n >= 16 && tx_n <= (LCD_W - 16) && ty_n >= 174 && ty_n <= 256) {
        if (g_ui_language == UI_LANG_TH) {
            dfplayer_play_track(89);
        } else {
            dfplayer_play_track(90);
        }
        s_schedule_detail_slot = -1;
        s_today_schedule_popup_drawn = false;
        s_popup_state = 4;
        force_redraw = true;
    } else {
        dfplayer_play_track(9);
        pending_page = PAGE_MENU;
    }
}

void ui_standby_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    ui_standby_handle_touch_modal(tx_n, ty_n);
}
