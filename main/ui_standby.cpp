#include "ui_core.h"
#include "ds3231.h"
#include "dispenser_scheduler.h"
#include "i2c_manager.h"
#include "dfplayer.h"
#include "offline_sync.h"
#include "wifi_sta.h"
#include "telegram_bot.h"
#include "ui_standby_thai_labels.h"
#include "ui_utf8_text.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Stub kept after the chrome-cache experiment was reverted (user
 * preferred the original full-repaint behaviour). Other translation
 * units still link against this symbol via ui_core.h, so leave it as
 * a no-op rather than touch every call site. */
void ui_standby_invalidate_chrome(void) {}

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

/* "Incomplete module with stale pills" alert (popup state 6).
 * Tracks which module index the warning is currently pointing at so
 * the touch handler can navigate to the right detail page when the
 * user taps. -1 = no alert currently shown. */
static int  s_incomplete_pill_idx = -1;
static bool s_incomplete_pill_drawn = false;

/* Boot-time "all empty — flush leftovers?" prompt (popup state 7) and
 * "clearing in progress" (state 8).
 *   s_boot_clear_offered  set true once we've shown the prompt this
 *                         boot — prevents re-prompting after the user
 *                         dismisses.
 *   s_boot_clear_seen     latched on first render after shadow loaded
 *                         so we only check the all-zero condition
 *                         once per boot. */
/* Both default to FALSE. The shadow-load gate in ui_standby_render_modal
 * decides whether to offer the boot-clear popup based on the actual
 * configured medication state (user 2026-05-15: "เช็คในระบบแล้วใช่ไหม
 * ว่ามีรายการยารอจ่ายอยู่หรือเปล่า — ไม่ใช่บังคับให้ล้าง"):
 *   - any med has slots configured → offered stays false (don't disturb
 *     a working schedule), seen=true to skip re-evaluation
 *   - all meds have empty slots → offered=true so user is forced through
 *     a clean-start flush before any further interaction
 * Window between boot and first shadow-load is tiny; if user manages to
 * tap during it they land in normal standby — acceptable because the
 * decision to lock or not is data-driven, not unconditional. */
static bool s_boot_clear_offered = false;
static bool s_boot_clear_seen    = false;
static bool s_boot_clear_drawn   = false;
static int  s_boot_clear_last_module_drawn = -2;

/* Read by the dispense scheduler so a slot-time match while the
 * unacknowledged boot-clear modal is up doesn't yank the user to
 * PAGE_CONFIRM_MEDS and bypass the lock. extern "C" so the symbol
 * matches the C-linkage declaration in ui_core.h — without it the C
 * side in dispenser_scheduler.c (which extern-declares the function
 * locally with C linkage) would fail to link against the C++-mangled
 * symbol the compiler would otherwise emit. */
extern "C" bool ui_standby_boot_clear_pending(void)
{
    return s_boot_clear_offered;
}

/* Popup state 9 — "กำลังจ่ายยา" overlay shown on standby while a
 * scheduled dispense is in progress. Painted ONCE per run (the
 * underlying state doesn't change while servos cycle), then cleared
 * when dispenser_is_busy() goes false. */
static bool s_dispensing_popup_drawn = false;

/* Popup state 10 — NETPIE pending approval. Shows when an external
 * write to the cloud shadow arrived; the operator must approve or
 * reject via the on-screen buttons (or Telegram /approve /reject).
 * Repaints only when the diff content changes (or popup state was 0). */
static bool s_pending_popup_drawn = false;
static uint32_t s_pending_popup_arrived_tick_last = 0;

/* Boot-time "configured-but-empty" alert. At boot, latch the mask of
 * modules whose name + slots are set but count == 0. Only THOSE modules
 * trigger the state-6 alert — a different module that gets emptied
 * later by a scheduled dispense does NOT inherit the latch, so the
 * user doesn't get a false "data missing" alarm right after a normal
 * dispense. Bits clear as user fixes each module (count > 0 or
 * config wiped). */
static bool    s_boot_empty_check_done = false;
static uint8_t s_boot_empty_mask       = 0;

/* One-shot audio guards — play voice prompt on FIRST appearance of the
 * popup, not on every render frame. Reset when the popup goes away so
 * a future re-entry plays again. Tracks (per user 2026-05-14 spec):
 *   104/105 — boot startup popup (state 7: "เริ่มต้นใช้งาน, กรุณาล้างยา")
 *   106/107 — clear-all in progress (state 8: "กำลังล้างยาทั้งหมด")
 *   108/109 — incomplete-module alert (state 6: "จัดการยาให้สมบูรณ์") */
static bool s_audio_played_state6 = false;
static bool s_audio_played_state7 = false;

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
    /* Full meal names per 2026-05-14 user spec — show "ก่อนอาหารเช้า"
     * etc., not the abbreviated "ก่อนเช้า". The renderer auto-shrinks
     * the font size if the longer string overruns the dose card. */
    if (strcmp(slot_label, "Before Breakfast") == 0) return "ก่อนอาหารเช้า";
    if (strcmp(slot_label, "After Breakfast") == 0)  return "หลังอาหารเช้า";
    if (strcmp(slot_label, "Before Lunch") == 0)     return "ก่อนอาหารกลางวัน";
    if (strcmp(slot_label, "After Lunch") == 0)      return "หลังอาหารกลางวัน";
    if (strcmp(slot_label, "Before Dinner") == 0)    return "ก่อนอาหารเย็น";
    if (strcmp(slot_label, "After Dinner") == 0)     return "หลังอาหารเย็น";
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
    (void)phase_color;

    if (!meal || !meal[0]) return;

    /* Build the full line: "Before Breakfast 08:00" / "Bedtime 21:00".
     * Phase (Before/After) is part of the same bold line per
     * 2026-05-14 user spec — they want the meal-phase visible. */
    char line[64] = "";
    if (phase && phase[0]) {
        if (time && time[0]) {
            snprintf(line, sizeof(line), "%s %s %s", phase, meal, time);
        } else {
            snprintf(line, sizeof(line), "%s %s", phase, meal);
        }
    } else {
        if (time && time[0]) {
            snprintf(line, sizeof(line), "%s %s", meal, time);
        } else {
            safe_copy(line, sizeof(line), meal);
        }
    }

    /* Try big-bold first. If the full "Before Breakfast 08:00" string is
     * too wide for the dose card, fall back to 18pt bold which always
     * fits. Both are bold per user request. */
    const GFXfont *font = &FreeSansBold24pt7b;
    int16_t main_w = gfx_text_width(line, font);
    int avail_w = content_right - content_left;
    if (main_w > avail_w) {
        font = &FreeSansBold18pt7b;
        main_w = gfx_text_width(line, font);
    }

    int16_t start_x = content_left + (((content_right - content_left) - main_w) / 2);
    if (start_x < content_left) start_x = content_left;

    fill_rect(content_left, baseline_y - 32, content_right - content_left, 44, bg);
    draw_string_gfx(start_x, baseline_y, line, main_color, bg, font);
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
        if (!include_empty_stock) {
            if (sh->med[i].count <= 0) continue;
        }
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
            safe_copy(buf, buf_len, (lang == UI_LANG_TH) ? "พลาดมื้อ" : "Missed");
            if (color) *color = THEME_BAD;
        } else {
            safe_copy(buf, buf_len, (lang == UI_LANG_TH) ? "เลยเวลาแล้ว" : "Passed");
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

        const char *name = sh->med[i].name[0] ? sh->med[i].name : ((lang == UI_LANG_TH) ? "ยังไม่ได้ตั้งชื่อ" : "Unnamed med");
        int stock = sh->med[i].count;
        if (stock > 0) {
            snprintf(lines[line_count], 96, "%d. %s (%d)", line_count + 1, name, stock);
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

/* ── Smooth seconds renderer ─────────────────────────────────────
 * Old path: draw_string_gfx_scaled() → draw_char_gfx_scaled() does
 *   1. fill_rect(cell, bg)          ← eye sees blank frame
 *   2. loop fg-pixels → fill_rect(2x2)  ← eye sees "build up"
 * That's ~hundreds of SPI transactions per char, each visible.
 *
 * New path: render the whole text into a PSRAM buffer (no SPI), then
 * push once with ui_draw_rgb_bitmap. Eye sees a single atomic flip.
 *
 * Buffer is allocated once on first call (PSRAM, freed never — it's
 * reused every second).  Plenty of slack: 200×120 fits :88:88 too. */
static uint16_t *s_ss_buf      = NULL;
static int       s_ss_buf_w    = 0;
static int       s_ss_buf_h    = 0;

static void draw_string_buffered(int16_t x, int16_t baseline_y,
                                 int16_t cell_w, int16_t cell_h,
                                 const char *str, uint16_t fg, uint16_t bg,
                                 const GFXfont *font, uint8_t scale)
{
    if (cell_w <= 0 || cell_h <= 0 || !str || !font) return;

    /* Lazy-alloc once.  Sized for the biggest user (seconds at 24pt×2). */
    const int CAP_W = 220;
    const int CAP_H = 130;
    if (!s_ss_buf) {
        s_ss_buf = (uint16_t *)heap_caps_malloc(CAP_W * CAP_H * 2,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ss_buf) {
            /* PSRAM exhausted — fall back to direct draw (flickery but
             * functionally correct). */
            draw_string_gfx_scaled(x, baseline_y, str, fg, bg, font, scale);
            return;
        }
        s_ss_buf_w = CAP_W;
        s_ss_buf_h = CAP_H;
    }
    if (cell_w > s_ss_buf_w || cell_h > s_ss_buf_h) {
        /* Caller asked for a bigger cell than the buffer can hold. */
        draw_string_gfx_scaled(x, baseline_y, str, fg, bg, font, scale);
        return;
    }

    /* Fill with bg.  RGB565 is big-endian over SPI for ST7796S — write
     * MSB first.  We'll splat bytes since fg/bg are 16-bit constants. */
    uint8_t bg_hi = (uint8_t)(bg >> 8);
    uint8_t bg_lo = (uint8_t)(bg & 0xFF);
    uint8_t *bp = (uint8_t *)s_ss_buf;
    for (int i = 0; i < cell_w * cell_h; i++) {
        bp[i * 2]     = bg_hi;
        bp[i * 2 + 1] = bg_lo;
    }

    /* Plot each glyph into the buffer (baseline-anchored coordinates,
     * same math as draw_char_gfx_scaled but writing to RAM instead of
     * piecewise SPI). The buffer's local origin is (x, baseline_y -
     * cell_h + something) — we work in (rel_x, rel_y) where rel_y=0 is
     * the buffer top. */
    int pen_x = 0;
    /* In the original renderer y is the BASELINE in screen coords. The
     * buffer's top is at (baseline_y - top_of_glyph_yoffset_at_scale).
     * For simplicity we assume the caller passes a cell that fully
     * encloses the glyph stack with the baseline aligned to the bottom
     * of caps.  We'll compute baseline within buf such that yo*scale
     * lands at the right place. */
    /* baseline_in_buf places the descender row near the bottom. Pick
     * an offset that leaves room for ascender above (yo is negative). */
    int baseline_in_buf = cell_h - scale * 2;  /* leave 2 px descender pad */
    while (*str) {
        unsigned char c = (unsigned char)*str++;
        if (c < font->first || c > font->last) continue;
        const GFXglyph *glyph = &font->glyph[c - font->first];
        uint16_t bo = glyph->bitmapOffset;
        uint8_t gw = glyph->width, gh = glyph->height;
        int8_t  xo = glyph->xOffset, yo = glyph->yOffset;
        uint8_t adv = glyph->xAdvance;
        const uint8_t *bitmap = font->bitmap;

        uint32_t bit_idx = 0;
        for (uint8_t gy = 0; gy < gh; gy++) {
            for (uint8_t gx = 0; gx < gw; gx++, bit_idx++) {
                uint8_t b = bitmap[bo + bit_idx / 8];
                bool on = (b >> (7 - (bit_idx & 7))) & 1;
                if (!on) continue;
                int px = pen_x + ((gx + xo) * scale);
                int py = baseline_in_buf + ((gy + yo) * scale);
                for (int dy = 0; dy < scale; dy++) {
                    int yy = py + dy;
                    if (yy < 0 || yy >= cell_h) continue;
                    for (int dx = 0; dx < scale; dx++) {
                        int xx = px + dx;
                        if (xx < 0 || xx >= cell_w) continue;
                        s_ss_buf[yy * cell_w + xx] = fg;
                    }
                }
            }
        }
        pen_x += adv * scale;
    }

    /* Compute screen Y so the in-buf baseline matches baseline_y. */
    int16_t screen_y = baseline_y - baseline_in_buf;
    ui_draw_rgb_bitmap(x, screen_y, cell_w, cell_h, s_ss_buf);
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

        /* Render via the buffered helper: builds the whole ":SS" cell
         * in PSRAM then pushes once. Eliminates the per-pixel SPI
         * transactions that made the digit flicker every second.
         *
         * cell_h sized to fit the glyph ONLY (cap top ≈ baseline-64 at
         * 24pt×2 + a small descender pad). If we passed the full
         * TIME_BLOCK_H, the buffer top would land above the card frame
         * line (y=16) and the buffer's bg color would paint over the
         * frame border, visually merging the digit into the frame. */
        const int16_t ss_cell_h = 70;
        draw_string_buffered(ss_x, TIME_BASELINE_Y, ss_area_w, ss_cell_h,
                             ss_with_colon, ST_TXT_TIME, ST_TIME_CARD,
                             &FreeSansBold24pt7b, TIME_SCALE);
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
                                               "ไม่มีการจ่ายยาต่อไป",
                                               ST_DOSE_MUTED, ST_DOSE_CARD, 28);
            } else {
                draw_string_centered(content_center_x, DOSE_Y + 52, "No upcoming schedule", ST_DOSE_MUTED, ST_DOSE_CARD, &FreeSans12pt7b);
            }
        } else {
            if (g_ui_language == UI_LANG_TH) {
                char line[64] = "";
                if (standby_build_next_dose_th_single_line(dose, line, sizeof(line))) {
                    /* Auto-shrink: "หลังอาหารกลางวัน 12:30" at size 36
                     * runs past the dose card. Step down until the
                     * rendered width fits the content area. */
                    int avail_w = content_right - content_left;
                    uint8_t target_h = 36;
                    while (target_h > 22 &&
                           ui_utf8_text_width_scaled_px(line, target_h) > avail_w) {
                        target_h -= 2;
                    }
                    int16_t top_y = DOSE_Y + (36 - target_h) / 2 + 24;
                    draw_utf8_centered_line_scaled(content_center_x, top_y, line, ST_UP_TEXT, ST_DOSE_CARD, target_h);
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
        safe_copy(wifi_name, sizeof(wifi_name), (g_ui_language == UI_LANG_TH) ? "ยังไม่ได้เชื่อม Wi-Fi" : "Wi-Fi not connected");
        safe_copy(web_url, sizeof(web_url), (g_ui_language == UI_LANG_TH) ? "แตะเพื่อตั้งค่า Wi-Fi" : "Tap to configure Wi-Fi");
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
    /* I2C health check + HARDWARE ERROR popup removed 2026-05-13.
     * Cached "err" flags stay false forever — RTC/PCA failures will
     * show through their own symptoms (servo doesn't move, clock
     * frozen) rather than nagging the user with a modal. */

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
        memcpy(hhmm, t_str, 5);
        hhmm[5] = '\0';
        memcpy(ss, t_str + 6, 2);
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

    /* Boot-time forced clear-all (user spec 2026-05-15, revised).
     *
     * Only offer the lock popup when NO medication is actually queued
     * for dispense — i.e. every module has slots == 0. The intent is
     * "first-use / fully-empty system" cleanup, not disrupting a working
     * schedule. If at least one med has slots configured the operator
     * already set things up and an unconditional flush would throw away
     * their setup.
     *
     * Either way we latch s_boot_clear_seen so this only fires once per
     * boot — subsequent shadow updates (e.g. NETPIE writes mid-session)
     * don't re-trigger the popup. */
    if (!s_boot_clear_seen && sh && sh->loaded) {
        bool any_scheduled = false;
        for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
            if (sh->med[i].slots != 0) { any_scheduled = true; break; }
        }
        s_boot_clear_offered = !any_scheduled;
        s_boot_clear_seen = true;
    }

    /* HARDWARE ERROR popup removed 2026-05-13.
     *
     * Popup state 6 = "incomplete module with stale pills" — at least
     * one cartridge has count > 0 but is missing name or schedule
     * slots. User must clear the cartridge before reconfiguring it
     * so old pills don't mix with new ones. */
    int incomplete_idx = -1;
    bool incomplete_has_pills = false;
    bool incomplete_needs_count = false;  /* true → boot-empty case: ask
                                            * user to fill in the count */
    if (sh && sh->loaded) {
        /* First render after shadow loaded — latch the boot empty-count
         * alert if any module is configured (name + slots) but has 0
         * pills. This catches the case where reboot finds modules in
         * "configured-but-empty" state and the user never gets a prompt
         * to add stock. */
        if (!s_boot_empty_check_done) {
            for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
                if (sh->med[i].name[0] != '\0' &&
                    sh->med[i].slots != 0 &&
                    sh->med[i].count == 0) {
                    s_boot_empty_mask |= (uint8_t)(1u << i);
                }
            }
            s_boot_empty_check_done = true;
        }

        for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
            bool has_name  = sh->med[i].name[0] != '\0';
            bool has_slots = sh->med[i].slots != 0;
            int  count     = sh->med[i].count;
            bool has_count = (count > 0);
            bool partial_config = (has_name != has_slots);  /* name XOR slots */
            bool stale_pills    = has_count && !(has_name && has_slots);
            /* boot_empty_now → THIS specific module was tagged at boot
             * AND still meets the condition. Per-module latch prevents
             * a scheduled dispense from triggering the alert on a
             * different module. */
            bool boot_empty_now = (s_boot_empty_mask & (1u << i)) &&
                                  has_name && has_slots && !has_count;
            /* Clear the per-module latch as soon as the user resolves it
             * (refilled, or wiped config). */
            if ((s_boot_empty_mask & (1u << i)) &&
                !(has_name && has_slots && !has_count)) {
                s_boot_empty_mask &= (uint8_t)~(1u << i);
            }
            if (incomplete_idx < 0 &&
                (partial_config || stale_pills || boot_empty_now)) {
                incomplete_idx = i;
                incomplete_has_pills = has_count;
                incomplete_needs_count = boot_empty_now &&
                                         !partial_config && !stale_pills;
            }
        }
    }
    s_incomplete_pill_idx = incomplete_idx;

    bool clear_all_running = dispenser_clear_all_active();
    bool dispense_running  = dispenser_is_busy();

    /* Edge-detect clear-all completion: was running last frame, now idle
     * → play the "เริ่มต้นใช้งานพร้อมแล้ว" voice (TH 112 / EN 113) once. */
    static bool s_prev_clear_all_running = false;
    static uint32_t s_clear_all_done_ms  = 0;
    if (s_prev_clear_all_running && !clear_all_running) {
        dfplayer_play_track((g_ui_language == UI_LANG_TH) ? 112 : 113);
        s_clear_all_done_ms = esp_log_timestamp();
    }
    s_prev_clear_all_running = clear_all_running;
    /* Within this window after clear-all-done, suppress state-6 voice
     * so 108/109 doesn't immediately cut off 112/113. 2.5 s is roughly
     * the length of the done prompt; state 6 popup itself still renders,
     * only its voice is held. */
    bool clear_all_done_recently =
        s_clear_all_done_ms != 0 &&
        (esp_log_timestamp() - s_clear_all_done_ms) < 2500;
    /* On a forced redraw we MUST paint the standby background even if a
     * popup will overlay it — otherwise we land on standby fresh from
     * another page (e.g. PAGE_CONFIRM_MEDS) with leftover pixels behind
     * the popup. The "DISPENSING…" overlay (state 9) is the canonical
     * case: user taps Confirm → page flips to standby → without this
     * fill the red confirm card is still visible behind the popup. */
    if (is_forced ||
        (s_popup_state != 1 && s_popup_state != 2 && s_popup_state != 3 &&
         s_popup_state != 4 && s_popup_state != 5 && s_popup_state != 6 &&
         s_popup_state != 7 && s_popup_state != 8 && s_popup_state != 9 &&
         s_popup_state != 10 &&
         incomplete_idx < 0 && !s_boot_clear_offered && !clear_all_running &&
         !dispense_running)) {
        draw_standby_page(is_forced, hhmm, ss, date_str, dose_str);
    }

    /* When the dispense finishes, drop popup state 9 and force a fresh
     * standby paint so the stale overlay is wiped. */
    if (s_popup_state == 9 && !dispense_running) {
        s_popup_state = 0;
        s_dispensing_popup_drawn = false;
        is_forced = true;
        draw_standby_page(is_forced, hhmm, ss, date_str, dose_str);
    }

    /* Popup state 9 — scheduled dispense in progress.
     * Highest priority: takes precedence over the schedule warning,
     * incomplete-pills alert, etc. The user just tapped "รับยา" and
     * needs immediate visual confirmation that the system is working
     * BEFORE the servo whirrs (which gets gated on
     * g_ui_dispensing_popup_painted in execute_dispense). */
    if (dispense_running) {
        if (!s_dispensing_popup_drawn || is_forced || s_popup_state != 9) {
            fill_round_rect_frame(40, 80, 400, 160, 16, THEME_OK, 0xFFFF);
            bool th = (g_ui_language == UI_LANG_TH);
            if (th) {
                draw_utf8_centered_line_scaled(LCD_W / 2, 120, "กำลังจ่ายยา",
                                               0xFFFF, THEME_OK, 36);
                draw_utf8_centered_line_scaled(LCD_W / 2, 185, "กรุณารอสักครู่",
                                               0xFFFF, THEME_OK, 24);
            } else {
                draw_string_centered(LCD_W / 2, 135, "DISPENSING...",
                                     0xFFFF, THEME_OK, &FreeSansBold24pt7b);
                draw_string_centered(LCD_W / 2, 195, "Please wait",
                                     0xFFFF, THEME_OK, &FreeSans18pt7b);
            }
            s_dispensing_popup_drawn = true;
            s_popup_state = 9;
            /* Signal to execute_dispense() that the popup is on screen —
             * unblocks the pre-servo paint-ack wait. */
            extern volatile bool g_ui_dispensing_popup_painted;
            g_ui_dispensing_popup_painted = true;
        }
        return;
    }

    /* If state was 6 but the condition cleared (user returned the pills),
     * reset the popup. */
    if (s_popup_state == 6 && incomplete_idx < 0) {
        s_popup_state = 0;
        s_incomplete_pill_drawn = false;
        s_audio_played_state6 = false;  /* re-arm voice prompt for next time */
        is_forced = true;
        draw_standby_page(is_forced, hhmm, ss, date_str, dose_str);
    }

    /* Render the incomplete-pills modal. Takes priority over the
     * generic schedule warning so the user fixes the stale-pill
     * situation first (it's more urgent — physical pills in the wrong
     * place).
     *
     * SUPPRESSED when the boot clear-all popup (state 7) is queued or
     * currently running (state 8): we must flush any leftover pills
     * BEFORE the user refills/reconfigures, otherwise new pills land
     * on top of old ones. Boot clear-all always wins at boot. */
    if (incomplete_idx >= 0 && !s_boot_clear_offered && !clear_all_running) {
        if (!s_incomplete_pill_drawn || is_forced || s_popup_state != 6) {
            fill_round_rect_frame(40, 50, 400, 220, 14, THEME_BAD, 0xFFFF);
            bool th = (g_ui_language == UI_LANG_TH);
            char head[64];
            if (th) {
                if (incomplete_has_pills) {
                    snprintf(head, sizeof(head), "ตลับที่ %d มียาค้าง", incomplete_idx + 1);
                    draw_utf8_centered_line_scaled(LCD_W / 2, 75, head, 0xFFFF, THEME_BAD, 30);
                    draw_utf8_centered_line_scaled(LCD_W / 2, 125,
                        "แต่ข้อมูลยายังไม่ครบ", 0xFFFF, THEME_BAD, 24);
                    draw_utf8_centered_line_scaled(LCD_W / 2, 165,
                        "ต้องคืนยาก่อนตั้งค่าใหม่", 0xFFFF, THEME_BAD, 24);
                    draw_utf8_centered_line_scaled(LCD_W / 2, 220,
                        "แตะที่หน้าจอเพื่อคืนยา", 0xFFFF, THEME_BAD, 22);
                } else if (incomplete_needs_count) {
                    snprintf(head, sizeof(head), "ตลับที่ %d ยังไม่ใส่ยา", incomplete_idx + 1);
                    draw_utf8_centered_line_scaled(LCD_W / 2, 75, head, 0xFFFF, THEME_BAD, 30);
                    draw_utf8_centered_line_scaled(LCD_W / 2, 130,
                        "ตั้งชื่อและมื้อแล้ว", 0xFFFF, THEME_BAD, 24);
                    draw_utf8_centered_line_scaled(LCD_W / 2, 170,
                        "แต่ยังไม่ระบุจำนวนเม็ดยา", 0xFFFF, THEME_BAD, 24);
                    draw_utf8_centered_line_scaled(LCD_W / 2, 220,
                        "แตะที่หน้าจอเพื่อเติมยา", 0xFFFF, THEME_BAD, 22);
                } else {
                    snprintf(head, sizeof(head), "ตลับที่ %d ข้อมูลไม่ครบ", incomplete_idx + 1);
                    draw_utf8_centered_line_scaled(LCD_W / 2, 75, head, 0xFFFF, THEME_BAD, 30);
                    draw_utf8_centered_line_scaled(LCD_W / 2, 130,
                        "กรุณาตั้งค่าให้ครบทุกช่อง", 0xFFFF, THEME_BAD, 24);
                    draw_utf8_centered_line_scaled(LCD_W / 2, 170,
                        "(ชื่อ + มื้อ + จำนวน)", 0xFFFF, THEME_BAD, 22);
                    draw_utf8_centered_line_scaled(LCD_W / 2, 220,
                        "แตะที่หน้าจอเพื่อตั้งค่า", 0xFFFF, THEME_BAD, 22);
                }
            } else {
                if (incomplete_has_pills) {
                    snprintf(head, sizeof(head), "Cartridge %d Has Pills", incomplete_idx + 1);
                    draw_string_centered(LCD_W / 2, 90, head,
                                         0xFFFF, THEME_BAD, &FreeSansBold18pt7b);
                    draw_string_centered(LCD_W / 2, 135, "but its setup is incomplete",
                                         0xFFFF, THEME_BAD, &FreeSans12pt7b);
                    draw_string_centered(LCD_W / 2, 170, "Return pills before reconfiguring",
                                         0xFFFF, THEME_BAD, &FreeSans12pt7b);
                    draw_string_centered(LCD_W / 2, 225, "Tap to return pills",
                                         0xFFFF, THEME_BAD, &FreeSans12pt7b);
                } else if (incomplete_needs_count) {
                    snprintf(head, sizeof(head), "Cartridge %d Is Empty", incomplete_idx + 1);
                    draw_string_centered(LCD_W / 2, 90, head,
                                         0xFFFF, THEME_BAD, &FreeSansBold18pt7b);
                    draw_string_centered(LCD_W / 2, 140, "Name and schedule are set,",
                                         0xFFFF, THEME_BAD, &FreeSans12pt7b);
                    draw_string_centered(LCD_W / 2, 175, "but pill count is 0",
                                         0xFFFF, THEME_BAD, &FreeSans12pt7b);
                    draw_string_centered(LCD_W / 2, 225, "Tap to refill",
                                         0xFFFF, THEME_BAD, &FreeSans12pt7b);
                } else {
                    snprintf(head, sizeof(head), "Cartridge %d Incomplete", incomplete_idx + 1);
                    draw_string_centered(LCD_W / 2, 90, head,
                                         0xFFFF, THEME_BAD, &FreeSansBold18pt7b);
                    draw_string_centered(LCD_W / 2, 140, "Please fill all required fields",
                                         0xFFFF, THEME_BAD, &FreeSans12pt7b);
                    draw_string_centered(LCD_W / 2, 175, "(name + slots + count)",
                                         0xFFFF, THEME_BAD, &FreeSans12pt7b);
                    draw_string_centered(LCD_W / 2, 225, "Tap to set up",
                                         0xFFFF, THEME_BAD, &FreeSans12pt7b);
                }
            }
            s_incomplete_pill_drawn = true;
            s_popup_state = 6;
            /* Voice prompt: "จัดการยาให้สมบูรณ์" (TH 108) / EN 109.
             * One-shot — re-armed when incomplete_idx goes back to -1.
             * Held back briefly if the clear-all-done prompt is still
             * playing so 112/113 isn't cut off mid-sentence. */
            if (!s_audio_played_state6 && !clear_all_done_recently) {
                dfplayer_play_track((g_ui_language == UI_LANG_TH) ? 108 : 109);
                s_audio_played_state6 = true;
            }
        }
        return;
    } else {
        s_incomplete_pill_drawn = false;
        s_audio_played_state6 = false;
    }

    /* Popup state 8: clear-all in progress. Refreshes every render to
     * show which module is currently being cleared. Takes priority
     * over state 7 (offer prompt) because the task is already running.
     *
     * Uses the SAME frame bounds as state 7 (40,50,400,220) so the
     * transition from state 7 → 8 paints over the old popup cleanly
     * with no leftover edges. */
    if (clear_all_running) {
        int cur = dispenser_clear_all_current_module();
        int pills_cur = dispenser_clear_all_pills_current();
        bool full_paint = (!s_boot_clear_drawn || is_forced || s_popup_state != 8);
        /* Repaint the mid-band whenever either the active module changes
         * OR the live pill count for the current module ticks. Without
         * the pills-changed check the count line would only update on
         * module rollover, hiding the IR counter live updates. */
        static int s_last_pills_drawn = -1;
        bool line_only  = (!full_paint &&
                           (cur != s_boot_clear_last_module_drawn ||
                            pills_cur != s_last_pills_drawn));

        if (full_paint) {
            /* First entry (or forced redraw) — paint the whole popup. */
            fill_round_rect_frame(40, 50, 400, 220, 14, THEME_PANEL, THEME_BORDER);
            bool th = (g_ui_language == UI_LANG_TH);
            if (th) {
                draw_utf8_centered_line_scaled(LCD_W / 2, 70,
                    "กำลังล้างยาทุกโมดูล", 0xFFFF, THEME_PANEL, 26);
                draw_utf8_centered_line_scaled(LCD_W / 2, 225,
                    "โปรดรอสักครู่…", 0xFFFF, THEME_PANEL, 20);
            } else {
                draw_string_centered(LCD_W / 2, 88, "Clearing all modules",
                                     0xFFFF, THEME_PANEL, &FreeSansBold18pt7b);
                draw_string_centered(LCD_W / 2, 240, "Please wait…",
                                     0xFFFF, THEME_PANEL, &FreeSans12pt7b);
            }
        }

        if (full_paint || line_only) {
            /* Mid-band: module index (large) + live pill count (small).
             * On full_paint draw fresh; on line_only wipe just the band
             * so header/footer don't blink. y=105..205 = 100 px tall. */
            bool th = (g_ui_language == UI_LANG_TH);
            if (line_only) {
                fill_rect(50, 105, 380, 100, THEME_PANEL);
            }
            char line[64];
            char pills_line[40] = "";
            if (cur < 0) {
                /* Not started yet (waiting for first module). */
                if (th) snprintf(line, sizeof(line), "กำลังเตรียม…");
                else    snprintf(line, sizeof(line), "Preparing…");
            } else if (cur < DISPENSER_MED_COUNT) {
                if (th) {
                    snprintf(line, sizeof(line), "โมดูลที่ %d / %d",
                             cur + 1, DISPENSER_MED_COUNT);
                    snprintf(pills_line, sizeof(pills_line),
                             "พบยา %d เม็ด", pills_cur);
                } else {
                    snprintf(line, sizeof(line), "Module %d / %d",
                             cur + 1, DISPENSER_MED_COUNT);
                    snprintf(pills_line, sizeof(pills_line),
                             "Found %d pills", pills_cur);
                }
            } else {
                /* Done — show total instead of "พบยา" */
                int total = dispenser_clear_all_pills_total();
                if (th) {
                    snprintf(line, sizeof(line), "เสร็จสิ้น");
                    snprintf(pills_line, sizeof(pills_line),
                             "รวม %d เม็ด", total);
                } else {
                    snprintf(line, sizeof(line), "Done");
                    snprintf(pills_line, sizeof(pills_line),
                             "Total %d pills", total);
                }
            }
            if (th) {
                draw_utf8_centered_line_scaled(LCD_W / 2, 130, line,
                                               0xFFFF, THEME_PANEL, 28);
                if (pills_line[0]) {
                    draw_utf8_centered_line_scaled(LCD_W / 2, 180, pills_line,
                                                   0xFFE0, THEME_PANEL, 22);
                }
            } else {
                draw_string_centered(LCD_W / 2, 145, line,
                                     0xFFFF, THEME_PANEL, &FreeSans18pt7b);
                if (pills_line[0]) {
                    draw_string_centered(LCD_W / 2, 185, pills_line,
                                         0xFFE0, THEME_PANEL, &FreeSans12pt7b);
                }
            }
            s_boot_clear_drawn = true;
            s_boot_clear_last_module_drawn = cur;
            s_last_pills_drawn = pills_cur;
            s_popup_state = 8;
            /* Signal to clear_all_task that the popup is now on screen —
             * task uses this to release its pre-servo wait. */
            extern volatile bool g_ui_clear_all_popup_painted;
            g_ui_clear_all_popup_painted = true;
        }
        return;
    }

    /* Popup state 7: boot-time forced flush prompt. Shown on every boot
     * (user spec 2026-05-15). Only ONE button (Clear) — user is forced
     * to acknowledge the safety flush, no skip path. */
    if (s_boot_clear_offered) {
        if (!s_boot_clear_drawn || is_forced) {
            fill_round_rect_frame(40, 50, 400, 220, 14, THEME_WARN, 0xFFFF);
            bool th = (g_ui_language == UI_LANG_TH);
            if (th) {
                draw_utf8_centered_line_scaled(LCD_W / 2, 75,
                    "เริ่มต้นใช้งาน", 0xFFFF, THEME_WARN, 30);
                draw_utf8_centered_line_scaled(LCD_W / 2, 130,
                    "ต้องล้างยาที่อาจค้างในโมดูล", 0xFFFF, THEME_WARN, 22);
                draw_utf8_centered_line_scaled(LCD_W / 2, 165,
                    "ก่อนเริ่มใช้งานเครื่อง", 0xFFFF, THEME_WARN, 22);
            } else {
                draw_string_centered(LCD_W / 2, 90, "Startup Check",
                                     0xFFFF, THEME_WARN, &FreeSansBold18pt7b);
                draw_string_centered(LCD_W / 2, 135, "Flush any leftover pills",
                                     0xFFFF, THEME_WARN, &FreeSans12pt7b);
                draw_string_centered(LCD_W / 2, 165, "in the modules before use",
                                     0xFFFF, THEME_WARN, &FreeSans12pt7b);
            }
            /* Single big "Clear" button centered — no skip. */
            fill_round_rect(140, 205, 200, 50, 12, THEME_BAD);
            if (th) {
                draw_utf8_centered_line_scaled(240, 218, "เริ่มล้างยา",
                                               0xFFFF, THEME_BAD, 28);
            } else {
                draw_string_centered(240, 237, "Clear Now",
                                     0xFFFF, THEME_BAD, &FreeSansBold18pt7b);
            }
            s_boot_clear_drawn = true;
            s_popup_state = 7;
            /* Voice prompt: "เริ่มต้นใช้งาน กรุณาล้างยาทั้งหมด" (TH 104) / EN 105.
             * Fires once per popup appearance — re-armed when state 7
             * is dismissed via the Clear button. */
            if (!s_audio_played_state7) {
                dfplayer_play_track((g_ui_language == UI_LANG_TH) ? 104 : 105);
                s_audio_played_state7 = true;
            }
        }
        return;
    } else {
        if (s_popup_state == 7 || s_popup_state == 8) {
            s_boot_clear_drawn = false;
            s_audio_played_state7 = false;
            s_popup_state = 0;
            is_forced = true;
            draw_standby_page(is_forced, hhmm, ss, date_str, dose_str);
        }
    }

    /* Popup state 10 — NETPIE pending approval. An external write to
     * the cloud shadow arrived (web widget / another client). Operator
     * must accept or reject before changes commit. Mandatory: the
     * popup stays up with NO timeout until /approve|/reject hits via
     * Telegram OR the operator taps a button below. */
    if (netpie_pending_active()) {
        netpie_pending_t pend;
        bool got = netpie_pending_get(&pend);
        bool need_paint = !s_pending_popup_drawn || is_forced ||
                         s_popup_state != 10 ||
                         (got && pend.arrived_tick != s_pending_popup_arrived_tick_last);
        if (need_paint) {
            fill_round_rect_frame(20, 30, 440, 260, 14, THEME_PANEL, 0xFFFF);
            bool th = (g_ui_language == UI_LANG_TH);
            if (th) {
                draw_utf8_centered_line_scaled(LCD_W / 2, 50,
                    "ตั้งค่าใหม่จาก NETPIE", 0xFFFF, THEME_PANEL, 28);
                draw_utf8_centered_line_scaled(LCD_W / 2, 92,
                    "กรุณายืนยันการบันทึก", 0xFFFF, THEME_PANEL, 22);
            } else {
                draw_string_centered(LCD_W / 2, 65, "NETPIE Changes Pending",
                                     0xFFFF, THEME_PANEL, &FreeSansBold18pt7b);
                draw_string_centered(LCD_W / 2, 100, "Approve to save the update",
                                     0xFFFF, THEME_PANEL, &FreeSans12pt7b);
            }

            /* Diff summary — up to a few lines so the operator sees what
             * actually changed before committing. */
            char summary[512];
            size_t slen = netpie_pending_format_summary_th(summary, sizeof(summary));
            (void)slen;
            /* Render first ~4 lines below the title at 18 px font. */
            int y = 130;
            const char *p = summary;
            for (int line = 0; line < 4 && *p; ++line) {
                const char *eol = strchr(p, '\n');
                char buf[160];
                size_t n = eol ? (size_t)(eol - p) : strlen(p);
                if (n >= sizeof(buf)) n = sizeof(buf) - 1;
                memcpy(buf, p, n);
                buf[n] = '\0';
                if (n > 0) {
                    /* Left-aligned, wrapped to popup width. */
                    ui_utf8_draw_text_scaled_px(40, y, buf, 0xFFFF, 18);
                }
                y += 24;
                if (!eol) break;
                p = eol + 1;
            }

            /* Two buttons: Reject (red, left) / Approve (green, right). */
            fill_round_rect(40,  236, 180, 44, 10, THEME_BAD);
            fill_round_rect(260, 236, 180, 44, 10, THEME_OK);
            if (th) {
                draw_utf8_centered_line_scaled(130, 248, "ปฏิเสธ",
                                               0xFFFF, THEME_BAD, 24);
                draw_utf8_centered_line_scaled(350, 248, "อนุมัติ",
                                               0xFFFF, THEME_OK, 24);
            } else {
                draw_string_centered(130, 264, "Reject",
                                     0xFFFF, THEME_BAD, &FreeSansBold18pt7b);
                draw_string_centered(350, 264, "Approve",
                                     0xFFFF, THEME_OK, &FreeSansBold18pt7b);
            }
            s_pending_popup_drawn = true;
            s_popup_state = 10;
            if (got) s_pending_popup_arrived_tick_last = pend.arrived_tick;
        }
        return;
    } else if (s_popup_state == 10) {
        /* Pending resolved externally (e.g. Telegram /approve). Clear
         * the overlay and force the chrome back. */
        s_pending_popup_drawn = false;
        s_popup_state = 0;
        is_forced = true;
        draw_standby_page(is_forced, hhmm, ss, date_str, dose_str);
    }

    if (s_netpie_sync_popup_until > 0 && now_ms < s_netpie_sync_popup_until) {
        if (s_popup_state != 3 || is_forced) {
            fill_round_rect_frame(60, 100, 360, 120, 15, THEME_OK, 0xFFFF);
            if (g_ui_language == UI_LANG_TH) {
                draw_standby_label((LCD_W - kThSyncOk1.width) / 2, 128, &kThSyncOk1);
                draw_standby_label((LCD_W - kThSyncOk2.width) / 2, 172, &kThSyncOk2);
            } else {
                /* Shortened — "Schedule Updated Successfully!" was wider
                 * than the 360-px popup at FreeSans12pt7b. */
                draw_string_centered(240, 145, "NETPIE SYNC", 0xFFFF, THEME_OK, &FreeSansBold18pt7b);
                draw_string_centered(240, 185, "Schedule Updated", 0xFFFF, THEME_OK, &FreeSans12pt7b);
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
                /* Lines trimmed to fit safely inside the 424-wide popup
                 * at FreeSansBold18pt7b / FreeSans12pt7b respectively.
                 * Old "Please set schedule via Netpie or Menu" + a long
                 * title pushed past the rounded-frame right edge. */
                draw_string_centered(LCD_W / 2, 102, "No Schedule Set", 0xFFFF, THEME_WARN, &FreeSansBold18pt7b);
                draw_string_centered(LCD_W / 2, 142, "Set via Netpie app", 0xFFFF, THEME_WARN, &FreeSans12pt7b);
                draw_string_centered(LCD_W / 2, 168, "or the on-screen Menu", 0xFFFF, THEME_WARN, &FreeSans12pt7b);
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

        (void)0; // ESP_LOGI("popup4", "enter render");
        int now_minutes = standby_time_to_minutes(hhmm);
        (void)0; // ESP_LOGI("popup4", "time_to_min OK now=%d", now_minutes);

        uint8_t missed_mask = dispenser_get_missed_slots();
        int missed_count = 0;
        for (int i = 0; i < 7; i++) {
            if (missed_mask & (1 << i)) missed_count++;
        }
        (void)0; // ESP_LOGI("popup4", "missed_mask OK %d", missed_count);

        int all_slots[7] = {0};
        int count = standby_collect_today_schedule_slots(all_slots, 7);
        (void)0; // ESP_LOGI("popup4", "collect_slots OK count=%d", count);
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

        (void)0; // ESP_LOGI("popup4", "frame fill start");
        fill_round_rect_frame(kSchedulePopupX, kSchedulePopupY, kSchedulePopupW, kSchedulePopupH, 16, THEME_PANEL, 0xFFFF);
        (void)0; // ESP_LOGI("popup4", "frame fill done");
        fill_round_rect(kScheduleCloseX, kScheduleCloseY, kScheduleCloseW, kScheduleCloseH, 8, 0xFFFF);
        draw_string_centered(kScheduleCloseX + (kScheduleCloseW / 2), kScheduleCloseY + 23, "X", THEME_PANEL, 0xFFFF, &FreeSans12pt7b);
        (void)0; // ESP_LOGI("popup4", "header done");

        if (g_ui_language == UI_LANG_TH) {
            (void)0; // ESP_LOGI("popup4", "TH render start");
            draw_utf8_centered_line_scaled(LCD_W / 2, 42, s_show_only_missed ? "มื้อที่พลาด" : "ตารางยาวันนี้", 0xFFFF, THEME_PANEL, 26);
            (void)0; // ESP_LOGI("popup4", "TH title done");
            
            char toggle_str[64];
            if (s_show_only_missed) {
                snprintf(toggle_str, sizeof(toggle_str), "ดูทั้งหมด");
            } else {
                snprintf(toggle_str, sizeof(toggle_str), "มื้อที่พลาด %d มื้อ", missed_count);
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
                draw_utf8_centered_line_scaled(LCD_W / 2, 140, s_show_only_missed ? "ไม่มีมื้อที่พลาด" : "วันนี้ไม่มีการจ่ายยา", THEME_TXT_MUTED, THEME_PANEL, 22);
            } else {
                draw_string_centered(240, 164, s_show_only_missed ? "No missed doses today" : "No dispensing schedule for today", THEME_TXT_MUTED, THEME_PANEL, &FreeSans12pt7b);
            }
        } else {
            (void)0; // ESP_LOGI("popup4", "drawing %d rows", s_schedule_visible_count);
            for (int i = 0; i < s_schedule_visible_count; ++i) {
                (void)0; // ESP_LOGI("popup4", "row %d slot=%d start", i, s_schedule_visible_slots[i]);
                draw_schedule_summary_row(kScheduleRowX, kScheduleRowY + (i * kScheduleRowH),
                                          kScheduleRowW, kScheduleRowH,
                                          s_schedule_visible_slots[i], now_minutes, g_ui_language);
                (void)0; // ESP_LOGI("popup4", "row %d done", i);
            }
        }

        (void)0; // ESP_LOGI("popup4", "all done");
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
    /* Popup state 8 = clear-all in progress. All taps ignored — user
     * cannot interrupt the safety flush. Popup self-dismisses when
     * the task finishes (s_clear_all_running goes false). */
    if (s_popup_state == 8 || dispenser_clear_all_active()) {
        return;
    }

    /* Popup state 9 = scheduled dispense in progress. Ignore all taps —
     * servo is moving, user cannot interrupt. Auto-dismisses when
     * dispenser_is_busy() returns false. */
    if (s_popup_state == 9 || dispenser_is_busy()) {
        return;
    }

    /* Boot-time forced clear-all (user spec 2026-05-14 + 2026-05-15).
     * Gate ONLY on s_boot_clear_offered — not also on s_popup_state==7.
     * The popup-state flag is set inside the render function on first
     * paint; gating the touch handler on it would let the user tap
     * BEFORE render fires, hitting some other touch path and navigating
     * away. With offered=true initialised at file scope, this branch
     * blocks taps from the very first standby touch after boot. */
    if (s_boot_clear_offered) {
        bool in_btn = (tx_n >= 140 && tx_n <= 340 && ty_n >= 205 && ty_n <= 255);
        if (in_btn) {
            s_boot_clear_offered = false;
            s_boot_clear_drawn = false;
            s_audio_played_state7 = false;
            s_boot_clear_last_module_drawn = -2;  /* force fresh paint on state 8 */
            if (dispenser_clear_all_start()) {
                s_popup_state = 8;
                /* "กำลังล้างยาทั้งหมด" voice — TH 106 / EN 107. */
                dfplayer_play_track((g_ui_language == UI_LANG_TH) ? 106 : 107);
            } else {
                s_popup_state = 0;
            }
            force_redraw = true;
            return;
        }
        return;  /* tap outside button — ignore, user must press Clear */
    }

    /* Popup state 10 — NETPIE pending approval. Two button rects:
     *   Reject:  x[40..220]   y[236..280]
     *   Approve: x[260..440]  y[236..280]
     * Tap outside the buttons is ignored — operator must explicitly
     * choose. Auto-dismisses when netpie_pending_active() goes false
     * (e.g. Telegram /approve cleared it). */
    if (s_popup_state == 10 && netpie_pending_active()) {
        bool on_reject  = (tx_n >=  40 && tx_n <= 220 && ty_n >= 236 && ty_n <= 280);
        bool on_approve = (tx_n >= 260 && tx_n <= 440 && ty_n >= 236 && ty_n <= 280);
        if (on_approve) {
            netpie_pending_approve();
            telegram_send_text(g_ui_language == UI_LANG_TH
                ? "ผู้ใช้กดอนุมัติบนหน้าจอแล้ว — บันทึกการตั้งค่าใหม่จาก NETPIE"
                : "Operator approved on touch screen — NETPIE changes saved");
            s_pending_popup_drawn = false;
            s_popup_state = 0;
            force_redraw = true;
            dfplayer_play_track(28);
        } else if (on_reject) {
            netpie_pending_reject();
            telegram_send_text(g_ui_language == UI_LANG_TH
                ? "ผู้ใช้กดปฏิเสธบนหน้าจอแล้ว — ค่าบน NETPIE ถูกย้อนกลับ"
                : "Operator rejected on touch screen — NETPIE values reverted");
            s_pending_popup_drawn = false;
            s_popup_state = 0;
            force_redraw = true;
            dfplayer_play_track(g_snd_button);
        }
        return;
    }

    /* Incomplete-module modal (state 6) — any tap navigates to that
     * module's detail page. If pills are inside, auto-open the
     * return-pill confirm dialog pre-filled with "ALL"; otherwise
     * just go to detail so the user can fill in missing fields. */
    if (s_popup_state == 6 && s_incomplete_pill_idx >= 0) {
        (void)tx_n; (void)ty_n;
        const netpie_shadow_t *sh_now = netpie_get_shadow();
        bool has_pills = (sh_now && sh_now->med[s_incomplete_pill_idx].count > 0);
        selected_med_idx    = s_incomplete_pill_idx;
        if (has_pills) {
            show_return_confirm = true;
            return_qty          = 100;          /* default to ALL */
        }
        s_popup_state       = 0;
        s_incomplete_pill_drawn = false;
        pending_page        = PAGE_SETUP_MEDS_DETAIL;
        force_redraw        = true;
        dfplayer_play_track(28);                /* confirm/forward sound */
        return;
    }

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

    /* Foot card (network status / IP) tap zone — covers the bottom
     * card so a tap on the WiFi/IP area opens the WiFi status page. */
    if (tx_n >= 16 && tx_n <= 320 && ty_n >= 256) {
        if (strcmp(s_ip, "0.0.0.0") != 0) pending_page = PAGE_WIFI_STATUS;
        else pending_page = PAGE_WIFI_SCAN;
        return;
    }

    /* Dose card (middle): tap opens "today's schedule" popup. */
    if (tx_n >= 16 && tx_n <= (LCD_W - 16) && ty_n >= 174 && ty_n <= 256) {
        if (g_ui_language == UI_LANG_TH) {
            dfplayer_play_track(89);
        } else {
            dfplayer_play_track(90);
        }
        s_schedule_detail_slot = -1;
        s_today_schedule_popup_drawn = false;
        s_popup_state = 4;
        force_redraw = true;
        return;
    }

    /* Time / date card (top): tap opens the main menu. The ft6336u
     * driver already filters phantom touches via 8 s boot-mute,
     * 4-sample press debounce, coord-stability check, and tap-spam
     * guard, so spurious presses landing here are rare in practice. */
    if (tx_n >= 16 && tx_n <= (LCD_W - 16) && ty_n >= 16 && ty_n <= 164) {
        dfplayer_play_track(9);
        pending_page = PAGE_MENU;
        return;
    }

    /* Anywhere else (tiny gaps between cards): do nothing. */
}

void ui_standby_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    ui_standby_handle_touch_modal(tx_n, ty_n);
}
