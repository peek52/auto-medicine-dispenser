#include "ui_core.h"
#include "netpie_mqtt.h"
#include "dispenser_scheduler.h"
#include "dfplayer.h"
#include "ds3231.h"
#include "ui_meds_thai_labels.h"
#include "ui_return_thai_labels.h"
#include "ui_utf8_text.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint32_t s_blink_tick   = 0;
static bool     s_blink_phase  = false;
static bool     s_validation_popup = false;
/* Which fields the user forgot when they pressed Save. The render
 * path reads these to compose a bilingual checklist popup. */
static bool     s_validation_missing_name  = false;
static bool     s_validation_missing_slots = false;
static bool     s_validation_missing_count = false;
/* Save-confirm popup — shown when user presses Back with a complete
 * 3-field entry, asking whether to keep (save) or discard the edits. */
static bool     s_save_confirm_popup = false;
/* Refill-or-clear popup — shown when display auto-navigated here after
 * a scheduled dispense failed (empty cartridge or IR saw no pill).
 * Two buttons: เติมยา → close popup, user adjusts count via +; ล้างข้อมูล
 * → wipe name + slots + count and go back to standby. Set by
 * display_clock.cpp when it consumes g_dispense_missed_nav_idx. */
static bool     s_refill_or_clear_popup = false;
static bool     s_refill_or_clear_drawn = false;
/* True once the refill-or-clear popup has fired for this entry. Stays
 * true until the user actually refills (count > 0) or explicitly taps
 * Clear. Back is blocked while this is true with count still 0 so the
 * user can't sneak out leaving an "I said I would refill" promise
 * unfulfilled — the popup re-fires forcing a deliberate choice. */
static bool     s_refill_pending        = false;
/* Clean-before-setup popup — fires when the user taps an UNCONFIGURED
 * slot (no name + no meal slots) on the grid. Forces a safety
 * acknowledgement that the cartridge and pill tube have been cleaned
 * before the operator starts entering a new medicine (user spec
 * 2026-05-18: "บังคับเด้งให้ล้างยาก่อนทุกครั้ง เพื่อความปลอดภัย").
 * Distinct from the refill-or-clear popup above: that one is for
 * configured slots that ran empty; this one is for slots that have
 * never been used. Two buttons: ยกเลิก → bounce back to the grid;
 * ล้างเสร็จแล้ว → close popup and let the user edit the detail page. */
static bool     s_clean_before_setup_popup = false;
static bool     s_clean_before_setup_drawn = false;
static netpie_med_t s_med_backup = {0};
static bool     s_med_snapshot_saved = false;

extern volatile int  ui_manual_disp_status;
extern volatile bool g_ui_dispensing_popup_painted;

/* Called by dispenser_scheduler when an IR-confirmed-empty event sets
 * count=0 (or when the dispense flow otherwise produces an authoritative
 * count). If the user happens to be on this med's detail page, update
 * the BACK-revert snapshot so pressing Back doesn't undo the system-set
 * value. Without this the user would have to manually press บันทึก to
 * keep the IR-cleared count = 0. */
extern "C" void ui_setup_meds_resync_backup_count(int med_idx, int new_count)
{
    if (med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return;
    /* Only update the snapshot if the user is currently editing THIS med.
     * Otherwise the snapshot is for a different med and resyncing would
     * corrupt the BACK-revert behavior for that one. */
    if (s_med_snapshot_saved && selected_med_idx == med_idx) {
        s_med_backup.count = new_count;
    }
}

/* Called by display_clock.cpp after auto-navigating to a med's detail
 * page following a failed scheduled dispense. Arms the refill-or-clear
 * popup so the first frame on the detail page shows the dialog. */
extern "C" void ui_setup_meds_arm_refill_or_clear(void)
{
    s_refill_or_clear_popup = true;
    s_refill_or_clear_drawn = false;
    s_refill_pending        = true;   /* must be resolved before leave */
}

// â”€â”€ File-scope modal timers (file-local only) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static uint32_t s_disp_done_tick = 0;
static uint32_t s_disp_fail_tick = 0;
static int      prev_return_qty  = 1;
static bool     prev_show_confirm = false;

#define MED_COUNT_BOX_X      302
#define MED_COUNT_BOX_Y      66
#define MED_COUNT_BOX_W      78
#define MED_COUNT_BOX_H      36
#define MED_COUNT_BOX_CX     (MED_COUNT_BOX_X + (MED_COUNT_BOX_W / 2))
#define MED_COUNT_BOX_TEXT_Y 88

#define RET_MINUS_X          66
#define RET_MINUS_Y          140
#define RET_MINUS_W          48
#define RET_MINUS_H          40
#define RET_QTY_X            122
#define RET_QTY_Y            140
#define RET_QTY_W            112
#define RET_QTY_H            40
#define RET_QTY_CX           (RET_QTY_X + (RET_QTY_W / 2))
#define RET_QTY_TEXT_Y       166
#define RET_PLUS_X           242
#define RET_PLUS_Y           140
#define RET_PLUS_W           48
#define RET_PLUS_H           40
#define RET_ALL_X            304
#define RET_ALL_Y            140
#define RET_ALL_W            96
#define RET_ALL_H            40
#define RET_ALL_CX           (RET_ALL_X + (RET_ALL_W / 2))
#define RET_ALL_TEXT_Y       164

static void draw_meds_label(int16_t x, int16_t y, const ui_label_bitmap_t *label)
{
    if (!label || !label->pixels) return;
    for (int16_t row = 0; row < label->height; ++row) {
        const uint16_t *src = label->pixels + (row * label->width);
        int16_t run_start = -1;
        for (int16_t col = 0; col <= label->width; ++col) {
            bool opaque = false;
            if (col < label->width) {
                uint16_t px = src[col];
                opaque = (px != THEME_PANEL && px != THEME_CARD && px != THEME_BG &&
                          px != THEME_BAD && px != THEME_OK && px != THEME_ACCENT &&
                          px != THEME_TXT_MUTED);
            }
            if (opaque && run_start < 0) run_start = col;
            else if (!opaque && run_start >= 0) {
                ui_draw_rgb_bitmap(x + run_start, y + row, col - run_start, 1, src + run_start);
                run_start = -1;
            }
        }
    }
}

static const ui_label_bitmap_t *med_module_title_label(int idx)
{
    static const ui_label_bitmap_t *labels[] = {&kThModule1,&kThModule2,&kThModule3,&kThModule4,&kThModule5,&kThModule6};
    return (idx >= 0 && idx < 6) ? labels[idx] : &kThModule1;
}

static const ui_label_bitmap_t *med_module_setup_label(int idx)
{
    static const ui_label_bitmap_t *labels[] = {&kThModule1Setup,&kThModule2Setup,&kThModule3Setup,&kThModule4Setup,&kThModule5Setup,&kThModule6Setup};
    return (idx >= 0 && idx < 6) ? labels[idx] : &kThModule1Setup;
}

static int count_selected_slots(uint8_t mask) {
    int c = 0;
    for (int i=0; i<8; i++) if (mask & (1<<i)) c++;
    return c;
}

static void draw_med_name_line(int16_t x, int16_t top_y, const char *name, uint16_t color, uint16_t bg, bool compact)
{
    if (!name || !name[0]) return;
    if (ui_utf8_has_non_ascii(name)) {
        fill_rect(x, top_y, compact ? 186 : 228, compact ? 18 : 24, bg);
        ui_utf8_draw_text(x, top_y - 1, name, color);
    } else {
        draw_string_gfx(x, compact ? (top_y + 15) : (top_y + 18), name, color, bg, &FreeSans9pt7b);
    }
}

static void fit_text_to_width(char *dst, size_t dst_cap, const char *src, int16_t max_w, bool utf8, const GFXfont *font)
{
    if (!dst || dst_cap == 0) return;
    ui_utf8_safe_truncate_copy(dst, dst_cap, src ? src : "");
    if (!dst[0]) return;

    if (utf8) {
        while (dst[0] && ui_utf8_text_width(dst) > max_w) {
            if (!ui_utf8_backspace(dst)) break;
        }
    } else {
        while (dst[0] && gfx_text_width(dst, font) > max_w) {
            size_t len = strlen(dst);
            if (len == 0) break;
            dst[len - 1] = '\0';
        }
    }

    if (src && strcmp(dst, src) != 0) {
        size_t len = strlen(dst);
        if (len + 3 < dst_cap) strcat(dst, "...");
    }
}

static void draw_meds_card_name(int16_t x, int16_t top_y, int16_t max_w, const char *name, uint16_t color, uint16_t bg)
{
    if (!name || !name[0]) return;

    char fitted[40];
    if (ui_utf8_has_non_ascii(name)) {
        fit_text_to_width(fitted, sizeof(fitted), name, max_w, true, NULL);
        fill_rect(x, top_y, max_w, 20, bg);
        ui_utf8_draw_text(x, top_y - 1, fitted, color);
    } else {
        fit_text_to_width(fitted, sizeof(fitted), name, max_w, false, &FreeSans12pt7b);
        fill_rect(x, top_y, max_w, 20, bg);
        draw_string_gfx(x, top_y + 18, fitted, color, bg, &FreeSans12pt7b);
    }
}

static void draw_med_count_value(int count)
{
    char cnt_str[16];
    snprintf(cnt_str, sizeof(cnt_str), "%d", count);
    fill_round_rect_frame(MED_COUNT_BOX_X, MED_COUNT_BOX_Y, MED_COUNT_BOX_W, MED_COUNT_BOX_H, 6, SB_COLOR_CARD, SB_COLOR_BORDER);
    draw_string_centered(MED_COUNT_BOX_CX, MED_COUNT_BOX_TEXT_Y, cnt_str, SB_COLOR_TXT_MAIN, SB_COLOR_CARD, &FreeSans12pt7b);
}

static void redraw_med_count_value_only(int count)
{
    char cnt_str[16];
    snprintf(cnt_str, sizeof(cnt_str), "%d", count);
    fill_round_rect(MED_COUNT_BOX_X + 1, MED_COUNT_BOX_Y + 1, MED_COUNT_BOX_W - 2, MED_COUNT_BOX_H - 2, 5, SB_COLOR_CARD);
    draw_string_centered(MED_COUNT_BOX_CX, MED_COUNT_BOX_TEXT_Y, cnt_str, SB_COLOR_TXT_MAIN, SB_COLOR_CARD, &FreeSans12pt7b);
}


static void draw_return_qty_value(int return_qty, int current_stock)
{
    char ret_str[16];
    if (return_qty == 100) snprintf(ret_str, sizeof(ret_str), "ALL");
    else snprintf(ret_str, sizeof(ret_str), "%d / %d", return_qty, current_stock);

    fill_round_rect_frame(RET_QTY_X, RET_QTY_Y, RET_QTY_W, RET_QTY_H, 6, THEME_BG, SB_COLOR_BORDER);
    draw_string_centered(RET_QTY_CX, RET_QTY_TEXT_Y, ret_str, SB_COLOR_TXT_MAIN, THEME_BG, &FreeSans12pt7b);
}

static void draw_return_all_button(void)
{
    fill_round_rect_frame(RET_ALL_X, RET_ALL_Y, RET_ALL_W, RET_ALL_H, 6, THEME_TXT_MUTED, THEME_TXT_MUTED);
    if (g_ui_language == UI_LANG_TH) {
        draw_meds_label(RET_ALL_X + ((RET_ALL_W - kThAll.width) / 2), 151, &kThAll);
    } else {
        draw_string_centered(RET_ALL_CX, RET_ALL_TEXT_Y, "ALL", 0x0000, THEME_TXT_MUTED, &FreeSans12pt7b);
    }
}

static int16_t ui_utf8_centered_x(int16_t x, int16_t w, const char *text)
{
    int16_t tw = ui_utf8_text_width(text);
    if (tw >= w) return x;
    return x + ((w - tw) / 2);
}

static void draw_slot_choice_button(int16_t x, int16_t y, int16_t w, int16_t h,
                                    bool active, bool blink_lit, const char *label_en, const char *label_th)
{
    // Dark blink phase uses a dim-green so the button is still clearly selected
    static const uint16_t kDimGreen = ST_RGB565(8, 100, 60);

    uint16_t bg, fg, border;
    if (active) {
        bg     = blink_lit ? THEME_OK : kDimGreen;
        fg     = 0xFFFF;
        border = THEME_OK;
    } else {
        bg     = THEME_BG;
        fg     = SB_COLOR_TXT_MAIN;
        border = SB_COLOR_BORDER;
    }

    fill_round_rect_frame(x, y, w, h, 7, bg, border);

    if (g_ui_language == UI_LANG_TH) {
        int16_t draw_x = ui_utf8_centered_x(x, w, label_th);
        int16_t draw_y = y + ((h - kUiUtf8FontLineHeight) / 2);
        ui_utf8_draw_text(draw_x, draw_y, label_th, fg);
    } else {
        draw_string_centered(x + (w / 2), y + 18, label_en, fg, bg, &FreeSans9pt7b);
    }
}

static void draw_slot_row_group(int16_t x, int16_t y, int16_t w, int16_t h,
                                const char *meal_en, const char *meal_th,
                                bool before_active, bool after_active)
{
    const int16_t label_w = 94;
    const int16_t gap = 8;
    const int16_t btn_w = 88;
    const int16_t btn_h = 24;
    const int16_t btn_y = y + 5;
    const int16_t before_x = x + label_w + gap;
    const int16_t after_x = before_x + btn_w + gap;

    fill_round_rect_frame(x, y, w, h, 10, SB_COLOR_CARD, SB_COLOR_BORDER);

    if (g_ui_language == UI_LANG_TH) {
        int16_t draw_x = ui_utf8_centered_x(x + 8, label_w - 16, meal_th);
        int16_t draw_y = y + ((h - kUiUtf8FontLineHeight) / 2);
        ui_utf8_draw_text(draw_x, draw_y, meal_th, SB_COLOR_PRIMARY);
    } else {
        draw_string_centered(x + (label_w / 2), y + 23, meal_en, SB_COLOR_PRIMARY, SB_COLOR_CARD, &FreeSans9pt7b);
    }

    /* Always pass blink_lit=true so selected slots render in bright
     * THEME_OK. The s_blink_phase animation was removed earlier (left
     * as a static `false` constant) — passing it here meant active
     * slots rendered in the dim-green fallback color the whole time,
     * which combined with partial-redraw overlap looked unstable. */
    draw_slot_choice_button(before_x, btn_y, btn_w, btn_h, before_active, true, "Before", "ก่อน");
    draw_slot_choice_button(after_x,  btn_y, btn_w, btn_h, after_active,  true, "After",  "หลัง");
}

static void draw_bedtime_slot_card(int16_t x, int16_t y, int16_t w, int16_t h, bool active)
{
    uint16_t bg = active ? THEME_OK : SB_COLOR_CARD;
    uint16_t fg = active ? 0xFFFF : SB_COLOR_TXT_MAIN;
    uint16_t border = active ? THEME_OK : SB_COLOR_BORDER;

    fill_round_rect_frame(x, y, w, h, 10, bg, border);

    if (g_ui_language == UI_LANG_TH) {
        const char *title = "ก่อนนอน";
        int16_t draw_x = ui_utf8_centered_x(x + 8, w - 16, title);
        ui_utf8_draw_text(draw_x, y + 18, title, fg);
    } else {
        draw_string_centered(x + (w / 2), y + 45, "Bedtime", fg, bg, &FreeSans12pt7b);
    }

    draw_string_centered(x + (w / 2), y + h - 12, active ? "ON" : "OFF", active ? 0xD7FA : SB_COLOR_TXT_MUTED, bg, &FreeSans9pt7b);
}

static void draw_slot_selector_panel(uint8_t slots)
{
    const int16_t row_x = 16;
    const int16_t row_w = 286;
    const int16_t row_h = 34;
    const int16_t row_gap = 6;
    /* Shifted from 100 to 114 to clear the +/- stock buttons that now
     * sit at y=60..108 after the top-bar bump. */
    const int16_t start_y = 114;
    const int16_t bed_x = 314;
    const int16_t bed_w = 150;
    const int16_t bed_h = (row_h * 3) + (row_gap * 2);

    draw_slot_row_group(row_x, start_y, row_w, row_h, "Breakfast", "เช้า",
                        (slots & (1 << 0)) != 0, (slots & (1 << 1)) != 0);
    draw_slot_row_group(row_x, start_y + row_h + row_gap, row_w, row_h, "Lunch", "เที่ยง",
                        (slots & (1 << 2)) != 0, (slots & (1 << 3)) != 0);
    draw_slot_row_group(row_x, start_y + ((row_h + row_gap) * 2), row_w, row_h, "Dinner", "เย็น",
                        (slots & (1 << 4)) != 0, (slots & (1 << 5)) != 0);
    draw_bedtime_slot_card(bed_x, start_y, bed_w, bed_h, (slots & (1 << 6)) != 0);
}

void ui_setup_meds_render(void)
{
    if (force_redraw) {
        fill_screen(THEME_BG);
        draw_top_bar_with_back(NULL);
        if (g_ui_language == UI_LANG_TH) draw_meds_label((LCD_W - kThTopMedSetup.width) / 2, 20, &kThTopMedSetup);
        else draw_string_centered(LCD_W / 2, 38, "Medicine Setup", THEME_TXT_MAIN, THEME_PANEL, &FreeSans12pt7b);

        const netpie_shadow_t *sh = netpie_get_shadow();

        const int card_w = 214;
        const int card_h = 78;
        const int left_x = 16;
        const int right_x = 250;
        const int start_y = 62;
        const int gap_y = 82;

        static const uint16_t module_colors[6] = {
            0x3DFF, // Cyan
            0x262B, // Green
            0xFE62, // Yellow
            0xFC87, // Orange
            0xEA53, // Pink
            0xAABE  // Purple
        };

        for (int i = 0; i < 6; i++) {
            int col = i % 2;
            int row = i / 2;
            int x = (col == 0) ? left_x : right_x;
            int y = start_y + row * gap_y;
            
            uint16_t mc = module_colors[i];

            fill_round_rect_frame(x, y, card_w, card_h, 10, SB_COLOR_CARD, mc);
            fill_rect(x, y, 8, card_h, mc);
            fill_round_rect_frame(x + 154, y + 10, 46, 56, 8, THEME_BG, mc);

            char title[20];
            snprintf(title, sizeof(title), "Module %d", i + 1);
            if (g_ui_language == UI_LANG_TH) draw_meds_label(x + 16, y + 8, med_module_title_label(i));
            else draw_string_gfx(x + 16, y + 24, title, mc, SB_COLOR_CARD, &FreeSans12pt7b);

            char med_name[64];
            if (strlen(sh->med[i].name) > 0) {
                snprintf(med_name, sizeof(med_name), "%s", sh->med[i].name);
            } else {
                snprintf(med_name, sizeof(med_name), "No name");
            }
            if (g_ui_language == UI_LANG_TH && strlen(sh->med[i].name) == 0) {
                draw_meds_label(x + 16, y + 32, &kThNoName);
            } else {
                draw_meds_card_name(x + 16, y + 28, 126, med_name, SB_COLOR_TXT_MAIN, SB_COLOR_CARD);
            }

            char info[24];
            if (g_ui_language == UI_LANG_TH) {
                snprintf(info, sizeof(info), "%d มื้อ", count_selected_slots(sh->med[i].slots));
            } else {
                snprintf(info, sizeof(info), "%d slots", count_selected_slots(sh->med[i].slots));
            }
            draw_string_gfx(x + 16, y + 68, info, SB_COLOR_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);

            char count_str[8];
            snprintf(count_str, sizeof(count_str), "%d", sh->med[i].count);
            draw_string_centered(x + 177, y + 50, count_str, THEME_TXT_MAIN, THEME_BG, &FreeSansBold18pt7b);
        }
        force_redraw = false;
    }

    // Always run the partial-redraw count diff so live count changes
    // (e.g. NETPIE shadow updates from another client) show up on this
    // page without needing a page change. Runs on every periodic tick
    // (PAGE_SETUP_MEDS is in page_needs_live_tick), so latency is the
    // 1 Hz render rate.
    static int s_prev_med_count[6] = { -999, -999, -999, -999, -999, -999 };
    const netpie_shadow_t *sh = netpie_get_shadow();
    const int left_x = 16;
    const int right_x = 250;
    const int start_y = 62;
    const int gap_y = 82;
    for (int i = 0; i < 6; i++) {
        if (sh->med[i].count == s_prev_med_count[i]) continue;
        int col = i % 2;
        int row = i / 2;
        int x = (col == 0) ? left_x : right_x;
        int y = start_y + row * gap_y;
        fill_round_rect(x + 154 + 2, y + 10 + 2, 46 - 4, 56 - 4, 6, THEME_BG);
        char count_str[8];
        snprintf(count_str, sizeof(count_str), "%d", sh->med[i].count);
        draw_string_centered(x + 177, y + 50, count_str, THEME_TXT_MAIN, THEME_BG, &FreeSansBold18pt7b);
        s_prev_med_count[i] = sh->med[i].count;
    }
}

void ui_setup_meds_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    if (ty_n < 60) {
        if (tx_n >= 0 && tx_n <= 130 && ty_n >= 0 && ty_n <= 52) {
            dfplayer_play_track(g_snd_button);
            pending_page = PAGE_MENU;
        }
    } else {
        const int card_w = 214;
        const int card_h = 78;
        const int left_x = 16;
        const int right_x = 250;
        const int start_y = 62;
        const int gap_y = 82;

        for (int i = 0; i < 6; i++) {
            int col = i % 2;
            int row = i / 2;
            int x = (col == 0) ? left_x : right_x;
            int y = start_y + row * gap_y;

            if (tx_n >= x && tx_n <= x + card_w &&
                ty_n >= y && ty_n <= y + card_h) {
                dfplayer_play_track(22 + i); // Tracks 22-27 for Modules 1-6
                selected_med_idx = i;
                pending_page = PAGE_SETUP_MEDS_DETAIL;
                force_redraw = true;
                /* Decide which (if any) popup to arm before the detail
                 * page paints. Three buckets:
                 *   1. Unconfigured slot (no name AND no slots)
                 *        → clean-before-setup safety popup. Forces the
                 *          operator to acknowledge the tube was cleaned
                 *          before they start punching in a new medicine.
                 *   2. Configured slot whose count has run to 0
                 *        → refill-or-clear popup (existing behaviour).
                 *   3. Configured slot with stock
                 *        → no popup, normal detail flow.
                 * The user explicitly asked (2026-05-18) for the safety
                 * popup on EVERY entry into an unconfigured slot, so it
                 * latches every time — there's no "skip" arm flag for
                 * unconfigured slots. */
                {
                    const netpie_shadow_t *sh_tap = netpie_get_shadow();
                    bool configured = (strlen(sh_tap->med[i].name) > 0) ||
                                      (sh_tap->med[i].slots != 0);
                    if (!configured) {
                        s_clean_before_setup_popup = true;
                        s_clean_before_setup_drawn = false;
                    } else if (sh_tap->med[i].count == 0) {
                        s_refill_or_clear_popup = true;
                        s_refill_or_clear_drawn = false;
                        s_refill_pending        = true;
                    }
                }
                break;
            }
        }
    }
}

void ui_setup_meds_detail_render(void)
{
    static netpie_shadow_t tp_prev_sh = {0};
    static int prev_disp_status = 0;

    /* FAST PATH: status just went 0→1 (user pressed Save/CONFIRM and
     * dispenser_manual_dispense flipped the flag).  Skip the heavy
     * full-page redraw and paint ONLY the "กำลังจ่ายยา" popup so
     * the user sees feedback within milliseconds.
     *
     * Frame size unified to (40,60,400,200) so this overwrites the
     * return-confirm dialog (same bounds) without leftover edges.
     * Previous (80,100,320,120) was smaller → return-confirm border
     * remained visible after transition (bug #2 in 2026-05-14 audit).
     *
     * Gate: don't take the fast path if validation or save-confirm
     * is also up — they take precedence and would be erased by us. */
    if (ui_manual_disp_status == 1 && prev_disp_status != 1 &&
        !s_validation_popup && !s_save_confirm_popup) {
        prev_disp_status = 1;
        fill_round_rect_frame(40, 60, 400, 200, 14, THEME_PANEL, THEME_BORDER);
        if (g_ui_language == UI_LANG_TH) {
            draw_meds_label((LCD_W - kThDispensing.width) / 2, 148, &kThDispensing);
        } else {
            draw_string_centered(LCD_W / 2, 165, "Dispensing...",
                                 THEME_TXT_MAIN, THEME_PANEL, &FreeSans18pt7b);
        }
        g_ui_dispensing_popup_painted = true;
        force_redraw = false;          /* don't double-paint this frame */
        return;
    }

    // Escalate timeout evaluator to bypass generic render clamping, but GATE it via parity sync to prevent legacy timestamp bypass
    if (ui_manual_disp_status == prev_disp_status) {
        if (ui_manual_disp_status == 2) {
            if ((xTaskGetTickCount() - s_disp_done_tick) > pdMS_TO_TICKS(2000)) {
                ui_manual_disp_status = 0;
                force_redraw = true; // Instantly resolves against the immediately following if-block!
            }
        } else if (ui_manual_disp_status == 3) {
            if ((xTaskGetTickCount() - s_disp_fail_tick) > pdMS_TO_TICKS(2000)) {
                ui_manual_disp_status = 0;
                force_redraw = true;
            }
        }
    }

    if (force_redraw) {
        /* Reset prev_disp_status on every full-page redraw entry so a
         * stale value from a previous session of this page can't
         * suppress the 0→1 fast-path next time the user opens it.
         * (Bug from 2026-05-14 audit: prev_disp_status is file-scope
         * static and persisted across page leaves.) */
        prev_disp_status = 0;

        // Save snapshot of current med data ONLY on first entry (not on every force_redraw)
        if (!s_med_snapshot_saved) {
            const netpie_shadow_t *sh_snap = netpie_get_shadow();
            s_med_backup = sh_snap->med[selected_med_idx];
            s_med_snapshot_saved = true;
            /* Suppress NETPIE publishes while the user is editing this
             * med detail page — +/- / slot toggle / rename should not
             * stream over MQTT on every tap. Local shadow + NVS still
             * update so the screen reflects each edit. The matching
             * pop + diff-publish happens on Save / Back in the touch
             * handler (see further below). */
            netpie_publish_inhibit_push();
        }

        fill_screen(THEME_BG);
        draw_top_bar_with_back(NULL);
        char page_title[32];
        snprintf(page_title, sizeof(page_title), "Module %d Setup", selected_med_idx + 1);
        if (g_ui_language == UI_LANG_TH) draw_meds_label((LCD_W / 2) - (med_module_setup_label(selected_med_idx)->width / 2), 20, med_module_setup_label(selected_med_idx));
        else draw_string_centered(LCD_W / 2, 38, page_title, THEME_TXT_MAIN, THEME_PANEL, &FreeSans12pt7b);

        // SAVE button top-right
        fill_round_rect_frame(330, 16, 140, 34, 8, THEME_OK, THEME_OK);
        if (g_ui_language == UI_LANG_TH) {
            int16_t save_tw = ui_utf8_text_width("บันทึก");
            ui_utf8_draw_text(400 - (save_tw / 2), 23, "บันทึก", 0xFFFF);
        } else {
            draw_string_centered(400, 39, "SAVE", 0xFFFF, THEME_OK, &FreeSans12pt7b);
        }

        const netpie_shadow_t *sh = netpie_get_shadow();
        int med_idx = selected_med_idx;
        
        /* Layout shifted down by ~14 px after the top bar grew from 44 to
         * 56 px (back button moved out of the panel edge). Previously the
         * name input box at y=52 and +/- buttons at y=46 ended up inside
         * the new bar and the + button overlapped the SAVE button. */
        fill_round_rect_frame(10, 66, 240, 36, 6, SB_COLOR_CARD, SB_COLOR_BORDER);
        bool has_name = strlen(sh->med[med_idx].name) > 0;
        if (g_ui_language == UI_LANG_TH && !has_name) {
            draw_meds_label(16, 74, &kThTapSetName);
        } else {
            if (has_name) draw_med_name_line(16, 71, sh->med[med_idx].name, SB_COLOR_PRIMARY, SB_COLOR_CARD, false);
            else draw_string_gfx(16, 90, "Tap to set name...", SB_COLOR_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
        }

        /* Larger stock buttons: 40×36 → 50×48 to meet ~44 px finger-tap
         * minimum. Keep the count-box position the same so the visual
         * layout stays balanced. */
        fill_round_rect_frame(252, 60, 50, 48, 6, THEME_BAD, SB_COLOR_BORDER);
        draw_string_centered(277, 90, "-", 0xFFFF, THEME_BAD, &FreeSans18pt7b);

        draw_med_count_value(sh->med[med_idx].count);

        fill_round_rect_frame(382, 60, 50, 48, 6, THEME_OK, SB_COLOR_BORDER);
        draw_string_centered(407, 90, "+", 0xFFFF, THEME_OK, &FreeSans18pt7b);

        draw_slot_selector_panel(sh->med[med_idx].slots);

        fill_rect(10, 230, 460, 1, SB_COLOR_BORDER);
        
        fill_round_rect_frame(16, 236, 448, 50, 10, THEME_ACCENT, THEME_ACCENT);
        if (g_ui_language == UI_LANG_TH) {
            draw_meds_label((LCD_W - kThReturnModeAccent.width) / 2, 236, &kThReturnModeAccent);
        } else {
            draw_string_centered(240, 268, "RETURN", 0x0000, THEME_ACCENT, &FreeSansBold18pt7b);
        }

        if (show_return_confirm) {
            fill_round_rect_frame(40, 60, 400, 200, 16, THEME_PANEL, THEME_TXT_MAIN);
            
            if (g_ui_language == UI_LANG_TH) {
                draw_meds_label((LCD_W - kThReturnModePanel.width) / 2, 68, &kThReturnModePanel);
            } else {
                char conf_msg[64];
                snprintf(conf_msg, sizeof(conf_msg), "RETURN MEDICINE");
                draw_string_centered(240, 92, conf_msg, THEME_TXT_MAIN, THEME_PANEL, &FreeSansBold18pt7b);
            }
            char module_msg[32];
            snprintf(module_msg, sizeof(module_msg), "MODULE %d", med_idx + 1);
            draw_string_centered(240, 116, module_msg, THEME_TXT_MUTED, THEME_PANEL, &FreeSans12pt7b);

            fill_rect(60, 124, 360, 2, THEME_BG);
            
            fill_round_rect_frame(RET_MINUS_X, RET_MINUS_Y, RET_MINUS_W, RET_MINUS_H, 6, THEME_BAD, SB_COLOR_BORDER);
            draw_string_centered(95, 164, "-", 0xFFFF, THEME_BAD, &FreeSansBold18pt7b);

            draw_return_qty_value(return_qty, sh->med[med_idx].count);

            fill_round_rect_frame(RET_PLUS_X, RET_PLUS_Y, RET_PLUS_W, RET_PLUS_H, 6, THEME_OK, SB_COLOR_BORDER);
            draw_string_centered(RET_PLUS_X + (RET_PLUS_W / 2), 164, "+", 0xFFFF, THEME_OK, &FreeSansBold18pt7b);

            draw_return_all_button();

            fill_round_rect_frame(60, 200, 160, 40, 8, THEME_BAD, THEME_BAD);
            if (g_ui_language == UI_LANG_TH) draw_meds_label(100, 208, &kThCancel);
            else draw_string_centered(140, 226, "CANCEL", 0xFFFF, THEME_BAD, &FreeSans12pt7b);
            
            fill_round_rect_frame(260, 200, 160, 40, 8, THEME_OK, THEME_OK);
            if (g_ui_language == UI_LANG_TH) draw_meds_label(300, 208, &kThConfirm);
            else draw_string_centered(340, 226, "CONFIRM", 0xFFFF, THEME_OK, &FreeSans12pt7b);
        }

        tp_prev_sh = *sh;
        prev_return_qty = return_qty;
        prev_show_confirm = show_return_confirm;
        force_redraw = false;
    } else {
        const netpie_shadow_t *sh = netpie_get_shadow();
        int med_idx = selected_med_idx;

        // Trap popup full redraw sequences
        if (show_return_confirm != prev_show_confirm) {
            prev_show_confirm = show_return_confirm;
            force_redraw = true;
            return;
        } else if (show_return_confirm) {
            // Active Popup Local Partial Updates
            if (return_qty != prev_return_qty) {
                draw_return_qty_value(return_qty, sh->med[med_idx].count);
                prev_return_qty = return_qty;
            }
        } else {
            // Background Local Partial Updates
            if (sh->med[med_idx].count != tp_prev_sh.med[med_idx].count) {
                redraw_med_count_value_only(sh->med[med_idx].count);
                tp_prev_sh.med[med_idx].count = sh->med[med_idx].count;
            }


            if (strcmp(sh->med[med_idx].name, tp_prev_sh.med[med_idx].name) != 0) {
                fill_round_rect_frame(10, 52, 240, 36, 6, SB_COLOR_CARD, SB_COLOR_BORDER);
                bool has_name = strlen(sh->med[med_idx].name) > 0;
                if (g_ui_language == UI_LANG_TH && !has_name) {
                    draw_meds_label(16, 60, &kThTapSetName);
                } else {
                    if (has_name) draw_med_name_line(16, 57, sh->med[med_idx].name, SB_COLOR_PRIMARY, SB_COLOR_CARD, false);
                    else draw_string_gfx(16, 76, "Tap to set name...", SB_COLOR_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
                }
                strncpy(tp_prev_sh.med[med_idx].name, sh->med[med_idx].name, sizeof(tp_prev_sh.med[med_idx].name));
            }

            /* Per-button partial redraw — smoother feel. Each bit that
             * flips redraws ONLY its 88×24 px button (or the bedtime
             * card). The label / row background are not touched, so the
             * user sees the tapped button transition without any flash
             * on adjacent buttons or labels. */
            uint8_t cur_slots = sh->med[med_idx].slots;
            uint8_t prev_slots = tp_prev_sh.med[med_idx].slots;
            uint8_t diff = (uint8_t)(cur_slots ^ prev_slots);
            if (diff) {
                /* Layout constants (must match draw_slot_row_group). */
                const int16_t row_x   = 16;
                const int16_t label_w = 94;
                const int16_t gap     = 8;
                const int16_t btn_w   = 88;
                const int16_t btn_h   = 24;
                const int16_t row_h   = 34;
                const int16_t row_gap = 6;
                /* Must match draw_slot_selector_panel — shifted from 100
                 * to 114 after the top-bar bump. Without keeping this in
                 * sync, tapping a Before/After button redraws it at the
                 * old y while the full-redraw position is 14 px lower,
                 * producing a "ghost" label at the wrong row. */
                const int16_t start_y = 114;
                const int16_t before_x = row_x + label_w + gap;       /* 118 */
                const int16_t after_x  = before_x + btn_w + gap;      /* 214 */
                const int16_t bed_x = 314, bed_w = 150;
                const int16_t bed_h = (row_h * 3) + (row_gap * 2);

                /* Per-meal redraw (only the buttons whose bit flipped). */
                struct { uint8_t before_bit, after_bit; int16_t row_y;
                         const char *en, *th; } meals[3] = {
                    { 0x01, 0x02, start_y,                        "Before", "ก่อน" },
                    { 0x04, 0x08, start_y + row_h + row_gap,      "Before", "ก่อน" },
                    { 0x10, 0x20, start_y + 2*(row_h + row_gap),  "Before", "ก่อน" },
                };
                const char *after_labels[2] = { "After", "หลัง" };
                for (int m = 0; m < 3; ++m) {
                    int16_t btn_y = meals[m].row_y + 5;
                    if (diff & meals[m].before_bit) {
                        draw_slot_choice_button(before_x, btn_y, btn_w, btn_h,
                                                (cur_slots & meals[m].before_bit) != 0,
                                                true,
                                                meals[m].en, meals[m].th);
                    }
                    if (diff & meals[m].after_bit) {
                        draw_slot_choice_button(after_x, btn_y, btn_w, btn_h,
                                                (cur_slots & meals[m].after_bit) != 0,
                                                true,
                                                after_labels[0], after_labels[1]);
                    }
                }
                if (diff & 0x40) {
                    draw_bedtime_slot_card(bed_x, start_y, bed_w, bed_h,
                                           (cur_slots & 0x40) != 0);
                }
                tp_prev_sh.med[med_idx].slots = cur_slots;
            }
        } // Closed 'else' logic for Background Local Partial Updates!

        // Repaint popup ONLY when its status actually changes, or when the
        // outer page does a full redraw. The old condition repainted the
        // popup every frame as long as status > 0, which made the
        // "กำลังจ่ายยา" panel visibly flicker.
        bool is_new_status = (ui_manual_disp_status != prev_disp_status);
        if (is_new_status || (force_redraw && ui_manual_disp_status > 0)) {
            prev_disp_status = ui_manual_disp_status;

            /* All dispense-status popups unified to (40,60,400,200)
             * bounds — same as return-confirm so transitions overwrite
             * cleanly. */
            if (ui_manual_disp_status == 1) {
                fill_round_rect_frame(40, 60, 400, 200, 14, THEME_PANEL, THEME_BORDER);
                if (g_ui_language == UI_LANG_TH) draw_meds_label((LCD_W - kThDispensing.width) / 2, 148, &kThDispensing);
                else draw_string_centered(LCD_W/2, 165, "Dispensing...", THEME_TXT_MAIN, THEME_PANEL, &FreeSans18pt7b);
                /* Signal to manual_dispense_task that the popup is now
                 * fully on screen — task will release its pre-servo wait
                 * and start driving the cup. */
                g_ui_dispensing_popup_painted = true;
            } else if (ui_manual_disp_status == 2) {
                fill_round_rect_frame(40, 60, 400, 200, 14, THEME_OK, THEME_BORDER);
                if (g_ui_language == UI_LANG_TH) {
                    draw_meds_label((LCD_W - kThDispenseOk1.width) / 2, 132, &kThDispenseOk1);
                    draw_meds_label((LCD_W - kThDispenseOk2.width) / 2, 175, &kThDispenseOk2);
                } else {
                    draw_string_centered(LCD_W/2, 150, "Dispense Complete", 0xFFFF, THEME_OK, &FreeSans18pt7b);
                    draw_string_centered(LCD_W/2, 190, "Finished", 0xFFFF, THEME_OK, &FreeSans12pt7b);
                }
                if (is_new_status) s_disp_done_tick = xTaskGetTickCount();
            } else if (ui_manual_disp_status == 3) {
                fill_round_rect_frame(40, 60, 400, 200, 14, THEME_BAD, THEME_BORDER);
                if (g_ui_language == UI_LANG_TH) {
                    draw_meds_label((LCD_W - kThDispenseFail1.width) / 2, 132, &kThDispenseFail1);
                    draw_meds_label((LCD_W - kThDispenseFail2.width) / 2, 175, &kThDispenseFail2);
                } else {
                    draw_string_centered(LCD_W/2, 150, "Dispense Failed", 0xFFFF, THEME_BAD, &FreeSans18pt7b);
                    draw_string_centered(LCD_W/2, 190, "Tap to retry", 0xFFFF, THEME_BAD, &FreeSans12pt7b);
                }
                if (is_new_status) s_disp_fail_tick = xTaskGetTickCount();
            } else if (ui_manual_disp_status == 0) {
                /* Dismiss — wipe the unified popup area only (not
                 * full screen). Force_redraw still set so the page
                 * underneath gets redrawn cleanly on this frame. */
                fill_round_rect(40, 60, 400, 200, 14, THEME_BG);
                force_redraw = true;
            }
        }
    }

    // Blink redraw removed — repainting the slot panel every 400ms caused
    // a perceptible whole-area flash whenever any value (count, name) was
    // changing concurrently, since the partial redraws layered. Slots are
    // already drawn with their selected-state colors at force_redraw and on
    // each slot/count/name change above; no animation is needed.
    (void)s_blink_phase; (void)s_blink_tick;

    /* ── Refill-or-clear popup: armed when display auto-navigated here
     *    after a scheduled dispense failed. Two buttons:
     *      เติมยา       → close popup, user adjusts count via existing UI
     *      ล้างข้อมูล   → wipe name + slots + count, return to standby
     *    Highest priority on the detail page — paints over normal UI.
     *    Bounds (40,50,400,220) match validation/save-confirm so
     *    transitions don't leave leftover edges. */

    /* ── Clean-before-setup popup. Fires when the operator taps an
     *    unconfigured module on the grid. Forces a tube-cleaning
     *    acknowledgement before the detail editor accepts input.
     *    Same bounds as the popups below so transitions are clean. */
    if (s_clean_before_setup_popup) {
        if (!s_clean_before_setup_drawn || force_redraw) {
            fill_round_rect_frame(40, 50, 400, 220, 14, THEME_BAD, 0xFFFF);
            bool th = (g_ui_language == UI_LANG_TH);
            char head[64];
            if (th) {
                snprintf(head, sizeof(head), "โมดูลที่ %d", selected_med_idx + 1);
                draw_utf8_centered_line_scaled(LCD_W / 2, 80, head,
                                               0xFFFF, THEME_BAD, 22);
                /* Title — shrunk + extra y-spacing so the descenders of
                 * "ล้างยาก่อน" don't kiss "ตั้งค่า" on the Thai font
                 * (user spec 2026-05-18: "ล้างยาก่อนตั้งค่ามันติดเกิน"). */
                draw_utf8_centered_line_scaled(LCD_W / 2, 125,
                    "ล้างยาก่อนตั้งค่า", 0xFFFF, THEME_BAD, 26);
                draw_utf8_centered_line_scaled(LCD_W / 2, 175,
                    "เพื่อความปลอดภัย", 0xFFFF, THEME_BAD, 20);
            } else {
                snprintf(head, sizeof(head), "Module %d", selected_med_idx + 1);
                draw_string_centered(LCD_W / 2, 90, head,
                                     0xFFFF, THEME_BAD, &FreeSans12pt7b);
                draw_string_centered(LCD_W / 2, 130, "Clean before setup",
                                     0xFFFF, THEME_BAD, &FreeSansBold18pt7b);
                draw_string_centered(LCD_W / 2, 175, "For safety",
                                     0xFFFF, THEME_BAD, &FreeSans12pt7b);
            }
            /* Two buttons: CANCEL (left, gray) + ล้างยา (right, green).
             * "ล้างยา" actually fires the servo flush — operator can't
             * reach the detail editor without running a clean cycle on
             * the cartridge first (user spec 2026-05-18). Cancel
             * bounces back to the grid. Neither plays a click sound. */
            fill_round_rect( 60, 220, 160, 44, 10, THEME_INACTIVE);
            fill_round_rect(260, 220, 160, 44, 10, THEME_OK);
            if (th) {
                draw_utf8_centered_line_scaled(140, 230, "ยกเลิก",
                                               0xFFFF, THEME_INACTIVE, 24);
                draw_utf8_centered_line_scaled(340, 230, "ล้างยา",
                                               0xFFFF, THEME_OK, 24);
            } else {
                draw_string_centered(140, 248, "Cancel", 0xFFFF,
                                     THEME_INACTIVE, &FreeSans12pt7b);
                draw_string_centered(340, 248, "Clean",  0xFFFF,
                                     THEME_OK, &FreeSans12pt7b);
            }
            s_clean_before_setup_drawn = true;
        }
        return;
    }

    if (s_refill_or_clear_popup) {
        if (!s_refill_or_clear_drawn || force_redraw) {
            fill_round_rect_frame(40, 50, 400, 220, 14, THEME_BAD, 0xFFFF);
            bool th = (g_ui_language == UI_LANG_TH);
            char head[64];
            if (th) {
                snprintf(head, sizeof(head), "โมดูลที่ %d ไม่มียาออก", selected_med_idx + 1);
                draw_utf8_centered_line_scaled(LCD_W / 2, 80, head,
                                               0xFFFF, THEME_BAD, 28);
                draw_utf8_centered_line_scaled(LCD_W / 2, 125,
                    "ต้องการเติมยาเพิ่ม?", 0xFFFF, THEME_BAD, 24);
                draw_utf8_centered_line_scaled(LCD_W / 2, 160,
                    "ถ้าไม่ จะล้างข้อมูลทั้งหมด", 0xFFFF, THEME_BAD, 20);
            } else {
                snprintf(head, sizeof(head), "Cartridge %d Empty", selected_med_idx + 1);
                draw_string_centered(LCD_W / 2, 95, head,
                                     0xFFFF, THEME_BAD, &FreeSansBold18pt7b);
                draw_string_centered(LCD_W / 2, 135, "Refill this cartridge?",
                                     0xFFFF, THEME_BAD, &FreeSans12pt7b);
                draw_string_centered(LCD_W / 2, 165, "No = clear all module data",
                                     0xFFFF, THEME_BAD, &FreeSans12pt7b);
            }
            /* Two buttons: CLEAR (left, gray) + REFILL (right, green) */
            fill_round_rect( 60, 200, 160, 44, 10, THEME_INACTIVE);
            fill_round_rect(260, 200, 160, 44, 10, THEME_OK);
            if (th) {
                draw_utf8_centered_line_scaled(140, 210, "ล้างข้อมูล",
                                               0xFFFF, THEME_INACTIVE, 22);
                draw_utf8_centered_line_scaled(340, 210, "เติมยา",
                                               0xFFFF, THEME_OK, 24);
            } else {
                draw_string_centered(140, 228, "Clear All", 0xFFFF,
                                     THEME_INACTIVE, &FreeSans12pt7b);
                draw_string_centered(340, 228, "Refill",    0xFFFF,
                                     THEME_OK, &FreeSans12pt7b);
            }
            s_refill_or_clear_drawn = true;
        }
        return;
    }

    /* ── Save-confirm popup: user pressed Back with all 3 fields
     *    filled. Asks "save changes or discard?" with two buttons.
     *    Bounds unified to (40,50,400,220) to match validation popup
     *    so transitions between them don't leave leftover edges. */
    static bool s_save_confirm_drawn = false;
    static bool s_save_confirm_audio_played = false;
    if (s_save_confirm_popup) {
        if (!s_save_confirm_drawn || force_redraw) {
            fill_round_rect_frame(40, 50, 400, 220, 14, THEME_PANEL, THEME_BORDER);
            bool th = (g_ui_language == UI_LANG_TH);
            if (th) {
                draw_utf8_centered_line_scaled(LCD_W / 2, 85,
                    "บันทึกการเปลี่ยนแปลง?", 0xFFFF, THEME_PANEL, 26);
                draw_utf8_centered_line_scaled(LCD_W / 2, 130,
                    "กดบันทึก = เก็บข้อมูล", 0xFFFF, THEME_PANEL, 20);
                draw_utf8_centered_line_scaled(LCD_W / 2, 160,
                    "กดทิ้ง = ย้อนกลับโดยไม่บันทึก", 0xFFFF, THEME_PANEL, 20);
            } else {
                draw_string_centered(LCD_W / 2, 100, "Save Changes?",
                                     0xFFFF, THEME_PANEL, &FreeSansBold18pt7b);
                draw_string_centered(LCD_W / 2, 140, "Save = keep new data",
                                     0xFFFF, THEME_PANEL, &FreeSans12pt7b);
                draw_string_centered(LCD_W / 2, 165, "Discard = revert changes",
                                     0xFFFF, THEME_PANEL, &FreeSans12pt7b);
            }
            /* Two buttons: DISCARD (left, red) + SAVE (right, green) */
            fill_round_rect(60, 200, 160, 44, 10, THEME_BAD);
            fill_round_rect(260, 200, 160, 44, 10, THEME_OK);
            if (th) {
                draw_utf8_centered_line_scaled(140, 210, "ทิ้ง",  0xFFFF, THEME_BAD, 24);
                draw_utf8_centered_line_scaled(340, 210, "บันทึก", 0xFFFF, THEME_OK,  24);
            } else {
                draw_string_centered(140, 228, "Discard", 0xFFFF, THEME_BAD, &FreeSans12pt7b);
                draw_string_centered(340, 228, "Save",    0xFFFF, THEME_OK,  &FreeSans12pt7b);
            }
            s_save_confirm_drawn = true;
            /* Voice prompt: "บันทึกการเปลี่ยนแปลง?" — TH 110 / EN 111.
             * One-shot per popup appearance, re-armed on dismiss. */
            if (!s_save_confirm_audio_played) {
                dfplayer_play_track((g_ui_language == UI_LANG_TH) ? 110 : 111);
                s_save_confirm_audio_played = true;
            }
        }
        return;
    } else {
        s_save_confirm_drawn = false;
        s_save_confirm_audio_played = false;
    }

    /* ── Validation popup: user pressed Save with missing fields. Lists
     * which fields are missing (Thai or English). Tap anywhere to dismiss.
     *
     * Thai text uses draw_utf8_centered_line_scaled (the only UTF-8-aware
     * draw in the codebase). FreeSans fonts don't have Thai glyphs, so
     * draw_string_gfx with Thai source would render blanks. */
    static bool s_validation_drawn_flag = false;
    if (s_validation_popup) {
        if (!s_validation_drawn_flag || force_redraw) {
            fill_round_rect_frame(40, 50, 400, 220, 14, THEME_WARN, 0xFFFF);
            bool th = (g_ui_language == UI_LANG_TH);

            /* Header */
            if (th) {
                draw_utf8_centered_line_scaled(LCD_W / 2, 75,
                    "กรอกข้อมูลไม่ครบ", 0xFFFF, THEME_WARN, 30);
            } else {
                draw_string_centered(LCD_W / 2, 90, "Missing Information",
                                     0xFFFF, THEME_WARN, &FreeSansBold18pt7b);
            }

            /* Per-field lines */
            int y = th ? 125 : 130;
            const int line_h = th ? 36 : 28;
            if (s_validation_missing_name) {
                if (th) {
                    draw_utf8_centered_line_scaled(LCD_W / 2, y,
                        "• กรุณาตั้งชื่อยา", 0xFFFF, THEME_WARN, 24);
                } else {
                    draw_string_centered(LCD_W / 2, y, "- Please set the medicine name",
                                         0xFFFF, THEME_WARN, &FreeSans12pt7b);
                }
                y += line_h;
            }
            if (s_validation_missing_slots) {
                if (th) {
                    draw_utf8_centered_line_scaled(LCD_W / 2, y,
                        "• กรุณาเลือกมื้อจ่ายยา", 0xFFFF, THEME_WARN, 24);
                } else {
                    draw_string_centered(LCD_W / 2, y, "- Please select dose times",
                                         0xFFFF, THEME_WARN, &FreeSans12pt7b);
                }
                y += line_h;
            }
            if (s_validation_missing_count) {
                if (th) {
                    draw_utf8_centered_line_scaled(LCD_W / 2, y,
                        "• กรุณาใส่จำนวนยาในโมดูล", 0xFFFF, THEME_WARN, 24);
                } else {
                    draw_string_centered(LCD_W / 2, y, "- Please enter pill count",
                                         0xFFFF, THEME_WARN, &FreeSans12pt7b);
                }
                y += line_h;
            }

            /* Footer */
            if (th) {
                draw_utf8_centered_line_scaled(LCD_W / 2, 245,
                    "แตะที่หน้าจอเพื่อปิด", 0xFFFF, THEME_WARN, 18);
            } else {
                draw_string_centered(LCD_W / 2, 250, "Tap to dismiss",
                                     0xFFFF, THEME_WARN, &FreeSans9pt7b);
            }
            s_validation_drawn_flag = true;
        }
    } else {
        s_validation_drawn_flag = false;
    }
}
/* Defensive cleanup — called from the page-transition watcher in
 * display_clock when the user leaves the meds-detail page via a path
 * other than Save/Back (e.g. scheduled-dispense Confirm popup yanks
 * them away). Mirrors what the Back handler does. Idempotent: a no-op
 * if no edit session is active. */
extern "C" void ui_setup_meds_end_edit_session_if_any(void)
{
    /* User exited via a path other than Save/Back (scheduled dose
     * Confirm popup yanked them away, watchdog, etc.). Mirror the
     * Back-button rule: if current state is complete, keep it; if
     * partial, wipe to fully empty. Never leave a module in a
     * half-finished partial state. */
    /* Drop the refill-or-clear popup latch — if it's still up we want
     * a clean slate next time the user (or auto-nav) reaches detail,
     * not a stale dialog from a previous failed dispense. */
    s_refill_or_clear_popup = false;
    s_refill_or_clear_drawn = false;
    s_refill_pending        = false;

    if (!s_med_snapshot_saved) return;
    const netpie_shadow_t *sh_cur = netpie_get_shadow();
    int idx = selected_med_idx;
    if (idx >= 0 && idx < DISPENSER_MED_COUNT) {
        bool has_name  = (sh_cur->med[idx].name[0] != '\0');
        bool has_slots = (sh_cur->med[idx].slots != 0);
        bool has_count = (sh_cur->med[idx].count > 0);
        bool complete = has_name && has_slots && has_count;
        if (!complete) {
            if (has_name)  netpie_shadow_update_med_name(idx + 1, "");
            if (has_count) netpie_shadow_update_count(idx + 1, 0);
            if (has_slots) netpie_shadow_update_med_slots(idx + 1, 0);
        }
    }
    netpie_publish_inhibit_pop();
    s_med_snapshot_saved = false;
}

void ui_setup_meds_detail_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    int med_idx = selected_med_idx;
    int current_stock = netpie_get_shadow()->med[med_idx].count;

    if (ui_manual_disp_status == 2 || ui_manual_disp_status == 3) {
        ui_manual_disp_status = 0; // Tap to dismiss
        force_redraw = true;
        return;
    } else if (ui_manual_disp_status == 1) {
        return; // Ignore touches while dispensing
    }

    /* Clean-before-setup popup: armed when an unconfigured slot was
     * tapped on the grid. Two buttons:
     *   left  (ยกเลิก) → bounce back to the grid, no editing
     *   right (ล้างยา) → fire dispenser_manual_dispense(idx, 100) so
     *                     the servo physically flushes anything left
     *                     in the cartridge tube; existing detail-page
     *                     "กำลังจ่ายยา" popup then takes over via
     *                     ui_manual_disp_status flipping to 1.
     * Neither button plays a click sound (user spec 2026-05-18:
     * "ไม่ต้องใส่เสียง"). Taps outside the buttons are ignored so the
     * operator can't accidentally close the popup. */
    if (s_clean_before_setup_popup) {
        bool in_left  = (tx_n >= 60  && tx_n <= 220 && ty_n >= 220 && ty_n <= 264);
        bool in_right = (tx_n >= 260 && tx_n <= 420 && ty_n >= 220 && ty_n <= 264);
        if (in_left) {
            s_clean_before_setup_popup = false;
            s_clean_before_setup_drawn = false;
            pending_page = PAGE_SETUP_MEDS;
            force_redraw = true;
            return;
        }
        if (in_right) {
            s_clean_before_setup_popup = false;
            s_clean_before_setup_drawn = false;
            /* Run the actual flush. qty=100 = "all" — servo cycles
             * until IR sees no more pills (or until the no-stock
             * timeout fires for an empty cartridge). Operator stays
             * on the detail page; the existing fast-path dispensing
             * popup will paint over the next frame. */
            dispenser_manual_dispense(selected_med_idx, 100);
            force_redraw = true;
            return;
        }
        return;   /* tap outside buttons — force a deliberate choice */
    }

    /* Refill-or-clear popup: armed via auto-nav after a failed
     * scheduled dispense. Two buttons, no escape — user must choose
     * either to refill (close popup, adjust count via existing UI)
     * or clear all (wipe name + slots + count, back to standby).
     * Spec 2026-05-14: "ห้ามมีโมดูลไหนที่ข้อมูลไม่ครบ" — leaving any
     * other way must wipe to fully empty. */
    if (s_refill_or_clear_popup) {
        bool in_left  = (tx_n >= 60  && tx_n <= 220 && ty_n >= 200 && ty_n <= 244);
        bool in_right = (tx_n >= 260 && tx_n <= 420 && ty_n >= 200 && ty_n <= 244);
        if (in_left) {
            /* CLEAR ALL — wipe name + slots + count, drop edit
             * session, return to standby. Update the backup snapshot
             * to the empty state so the page-leave watcher doesn't
             * second-guess us. */
            netpie_shadow_update_med_name(med_idx + 1, "");
            netpie_shadow_update_med_slots(med_idx + 1, 0);
            netpie_shadow_update_count(med_idx + 1, 0);
            /* Only pop if the matching push at line ~535 actually
             * happened — otherwise an unpushed pop drains the inhibit
             * counter prematurely and the NEXT real edit session won't
             * suppress publishes. */
            if (s_med_snapshot_saved) {
                netpie_publish_inhibit_pop();
                s_med_backup.name[0] = '\0';
                s_med_backup.slots   = 0;
                s_med_backup.count   = 0;
            }
            s_med_snapshot_saved = false;
            s_refill_or_clear_popup = false;
            s_refill_or_clear_drawn = false;
            s_refill_pending        = false;  /* explicitly chose Clear */
            dfplayer_play_track(12);   /* cancel sound */
            pending_page = PAGE_STANDBY;
            force_redraw = true;
            return;
        }
        if (in_right) {
            /* REFILL — just close the popup. User continues on the
             * detail page and adjusts the pill count via the + button,
             * then presses Save / Back as normal. s_refill_pending
             * stays true until count actually goes > 0 (or the user
             * later picks Clear). */
            s_refill_or_clear_popup = false;
            s_refill_or_clear_drawn = false;
            dfplayer_play_track(28);   /* confirm sound */
            force_redraw = true;
            return;
        }
        /* Tap outside the buttons — ignore. Force a deliberate choice. */
        return;
    }

    /* Validation popup is shown — any tap dismisses it, no other
     * action processed this frame. Lets the user fix the missing
     * field on the next tap rather than accidentally triggering
     * something else through the popup. */
    if (s_validation_popup) {
        s_validation_popup = false;
        s_validation_missing_name  = false;
        s_validation_missing_slots = false;
        s_validation_missing_count = false;
        force_redraw = true;
        return;
    }

    /* Save-confirm popup: handle the two buttons (left=Discard,
     * right=Save). Any tap outside the buttons is ignored — user
     * must make a deliberate choice. */
    if (s_save_confirm_popup) {
        bool in_left  = (tx_n >= 60  && tx_n <= 220 && ty_n >= 200 && ty_n <= 244);
        bool in_right = (tx_n >= 260 && tx_n <= 420 && ty_n >= 200 && ty_n <= 244);
        if (in_left) {
            /* DISCARD — revert + leave. */
            const netpie_shadow_t *sh_cur = netpie_get_shadow();
            if (strcmp(sh_cur->med[med_idx].name, s_med_backup.name) != 0)
                netpie_shadow_update_med_name(med_idx + 1, s_med_backup.name);
            if (sh_cur->med[med_idx].count != s_med_backup.count)
                netpie_shadow_update_count(med_idx + 1, s_med_backup.count);
            if (sh_cur->med[med_idx].slots != s_med_backup.slots)
                netpie_shadow_update_med_slots(med_idx + 1, s_med_backup.slots);
            netpie_publish_inhibit_pop();
            dfplayer_play_track(12);   /* cancel sound */
            s_save_confirm_popup = false;
            s_med_snapshot_saved = false;
            pending_page = PAGE_SETUP_MEDS;
            force_redraw = true;
            return;
        }
        if (in_right) {
            /* SAVE — diff the post-edit shadow against the on-entry
             * backup, then send a single Telegram message summarising
             * what actually changed during this edit session. User spec
             * 2026-05-15: "กดบันทึกอย่างเดียวก็พอ ว่าทำอะไรไป". */
            const netpie_shadow_t *sh_check = netpie_get_shadow();
            netpie_publish_inhibit_pop();
            netpie_shadow_commit_med_diff(med_idx + 1, &s_med_backup);
            dfplayer_play_track(14);
            s_save_confirm_popup = false;

            /* Take the snapshot needed for diff BEFORE clearing the flag. */
            const netpie_med_t *now = &sh_check->med[med_idx];
            const netpie_med_t *was = &s_med_backup;
            bool name_changed  = strcmp(was->name, now->name) != 0;
            bool count_changed = was->count != now->count;
            bool slots_changed = was->slots != now->slots;
            s_med_snapshot_saved = false;

            char time_str[16] = "--:--";
            ds3231_get_time_str(time_str, sizeof(time_str));
            bool th = (g_ui_language == UI_LANG_TH);

            /* Build the change list. If nothing actually changed (user
             * pressed Save without edits), skip the Telegram entirely —
             * spam-prevention. */
            if (name_changed || count_changed || slots_changed) {
                char old_slots[120] = "", new_slots[120] = "";
                if (slots_changed) {
                    dispenser_format_slots_to_names(was->slots, old_slots, sizeof(old_slots));
                    dispenser_format_slots_to_names(now->slots, new_slots, sizeof(new_slots));
                }

                char msg[600];
                int off = 0;
                const char *disp_name = now->name[0] ? now->name :
                                        (was->name[0] ? was->name : (th ? "ไม่มีชื่อ" : "Unnamed"));

                off += snprintf(msg + off, sizeof(msg) - off,
                    th ? "📋 บันทึกข้อมูลยา\nเวลา: %s\nโมดูล: %d (%s)\n"
                       : "📋 Medication setup saved\nTime: %s\nModule: %d (%s)\n",
                    time_str, med_idx + 1, disp_name);

                if (name_changed) {
                    const char *o = was->name[0] ? was->name : (th ? "(ว่าง)" : "(empty)");
                    const char *n = now->name[0] ? now->name : (th ? "(ว่าง)" : "(empty)");
                    off += snprintf(msg + off, sizeof(msg) - off,
                        th ? "• ชื่อยา: %s → %s\n" : "• Name: %s → %s\n", o, n);
                }
                if (count_changed) {
                    off += snprintf(msg + off, sizeof(msg) - off,
                        th ? "• จำนวน: %d → %d เม็ด (%+d)\n"
                           : "• Count: %d → %d pills (%+d)\n",
                        was->count, now->count, now->count - was->count);
                }
                if (slots_changed) {
                    const char *o = old_slots[0] ? old_slots : "-";
                    const char *n = new_slots[0] ? new_slots : "-";
                    off += snprintf(msg + off, sizeof(msg) - off,
                        th ? "• มื้อจ่าย: %s → %s\n" : "• Slots: %s → %s\n", o, n);
                }

                extern void dispenser_telegram_photo_msg(const char *msg);
                dispenser_telegram_photo_msg(msg);
            }

            pending_page = PAGE_SETUP_MEDS;
            force_redraw = true;
            return;
        }
        /* Tap outside the buttons — ignore. */
        return;
    }

    if (show_return_confirm) {
        // ... handled below
    } else if (tx_n >= 0 && tx_n <= 130 && ty_n >= 0 && ty_n <= 52) {
        /* Refill-or-clear unresolved? If the user landed here via the
         * post-dispense auto-nav, tapped "เติมยา" (Refill) but never
         * actually added any pills, Back must NOT silently wipe — re-show
         * the popup so they make a deliberate choice (refill or clear).
         * Spec 2026-05-14: ห้ามหลุดออกจากหน้านี้แบบเงียบ ๆ ด้วยข้อมูลไม่ครบ. */
        if (s_refill_pending) {
            const netpie_shadow_t *sh_now = netpie_get_shadow();
            if (sh_now->med[med_idx].count == 0) {
                s_refill_or_clear_popup = true;
                s_refill_or_clear_drawn = false;
                dfplayer_play_track(g_snd_button);
                force_redraw = true;
                return;
            }
            /* Count is non-zero now → user did refill. Clear the latch
             * and fall through to normal Back handling. */
            s_refill_pending = false;
        }
        /* Back behavior (user spec 2026-05-14):
         *   - No changes since entry         → silent leave (no popup).
         *   - Complete (all 3 fields filled) → save-confirm popup.
         *   - Partial / empty                → WIPE TO FULLY EMPTY.
         *
         * The wipe enforces the "ห้ามมีโมดูลไหนที่ข้อมูลไม่ครบ" rule:
         * a module is either fully configured or fully blank — never
         * partial. So if user enters a module with stale data, clears
         * the name, and presses Back, the module ends up empty
         * (instead of reverting to the stale state). */
        const netpie_shadow_t *sh_cur = netpie_get_shadow();
        bool has_name  = (sh_cur->med[med_idx].name[0] != '\0');
        bool has_slots = (sh_cur->med[med_idx].slots != 0);
        bool has_count = (sh_cur->med[med_idx].count > 0);

        bool no_changes = (strcmp(sh_cur->med[med_idx].name, s_med_backup.name) == 0)
                       && (sh_cur->med[med_idx].count == s_med_backup.count)
                       && (sh_cur->med[med_idx].slots == s_med_backup.slots);

        if (no_changes) {
            /* Nothing changed — just leave, don't bother asking. */
            netpie_publish_inhibit_pop();
            dfplayer_play_track(g_snd_button);
            s_validation_popup = false;
            s_validation_missing_name  = false;
            s_validation_missing_slots = false;
            s_validation_missing_count = false;
            s_med_snapshot_saved = false;
            pending_page = PAGE_SETUP_MEDS;
            force_redraw = true;
            return;
        }

        if (has_name && has_slots && has_count) {
            /* Complete entry — ask before discarding. */
            s_save_confirm_popup = true;
            dfplayer_play_track(g_snd_button);
            force_redraw = true;
            return;
        }

        /* Partial or empty entry — wipe everything to a clean blank
         * state so the module never holds stale partial data. */
        netpie_publish_inhibit_pop();
        if (sh_cur->med[med_idx].name[0] != '\0')
            netpie_shadow_update_med_name(med_idx + 1, "");
        if (sh_cur->med[med_idx].count != 0)
            netpie_shadow_update_count(med_idx + 1, 0);
        if (sh_cur->med[med_idx].slots != 0)
            netpie_shadow_update_med_slots(med_idx + 1, 0);

        dfplayer_play_track(g_snd_button);
        s_validation_popup = false;
        s_validation_missing_name  = false;
        s_validation_missing_slots = false;
        s_validation_missing_count = false;
        s_med_snapshot_saved = false; // Reset for next entry
        pending_page = PAGE_SETUP_MEDS;
        force_redraw = true;
        return;
    } else if (ty_n >= 14 && ty_n <= 52 && tx_n >= 330 && tx_n <= 470) {
        /* SAVE button — strict validation per user spec 2026-05-14.
         * ALL THREE of (name / slots / count) must be filled, otherwise
         * a bilingual popup tells the user exactly what's missing and
         * the save is aborted (user stays on the page). When everything
         * is filled, the save commits + the camera grabs a confirmation
         * photo + Telegram fires. */
        const netpie_shadow_t *sh_check = netpie_get_shadow();
        bool has_name  = (sh_check->med[med_idx].name[0] != '\0');
        bool has_slots = (sh_check->med[med_idx].slots != 0);
        bool has_count = (sh_check->med[med_idx].count > 0);

        if (!(has_name && has_slots && has_count)) {
            /* Missing at least one field — fire the validation popup
             * and abort the save. Popup renders on the next frame
             * via s_validation_popup flag (see render path). */
            s_validation_missing_name  = !has_name;
            s_validation_missing_slots = !has_slots;
            s_validation_missing_count = !has_count;
            s_validation_popup = true;
            /* No audio on validation fail — the visual popup IS the
             * warning. Track 12 = Cancel voice which sounded like the
             * save was rejected, confusing the user. */
            force_redraw = true;
            return;
        }

        /* Validated OK — commit + diff-summary Telegram + leave. */
        netpie_publish_inhibit_pop();
        netpie_shadow_commit_med_diff(med_idx + 1, &s_med_backup);
        dfplayer_play_track(14);
        s_validation_popup = false;
        s_refill_pending = false;     // Save committed fulfills the promise

        /* Diff-summary: compare current state against on-entry backup,
         * report only what changed. User spec 2026-05-15
         * "กดบันทึกอย่างเดียวก็พอ ว่าทำอะไรไป". Capture diff fields
         * before clearing s_med_snapshot_saved. */
        {
            const netpie_med_t *now = &sh_check->med[med_idx];
            const netpie_med_t *was = &s_med_backup;
            bool name_changed  = strcmp(was->name, now->name) != 0;
            bool count_changed = was->count != now->count;
            bool slots_changed = was->slots != now->slots;
            s_med_snapshot_saved = false;

            if (name_changed || count_changed || slots_changed) {
                char time_str[16] = "--:--";
                ds3231_get_time_str(time_str, sizeof(time_str));
                bool th = (g_ui_language == UI_LANG_TH);

                char old_slots[120] = "", new_slots[120] = "";
                if (slots_changed) {
                    dispenser_format_slots_to_names(was->slots, old_slots, sizeof(old_slots));
                    dispenser_format_slots_to_names(now->slots, new_slots, sizeof(new_slots));
                }

                char msg[600];
                int off = 0;
                const char *disp_name = now->name[0] ? now->name :
                                        (was->name[0] ? was->name : (th ? "ไม่มีชื่อ" : "Unnamed"));

                off += snprintf(msg + off, sizeof(msg) - off,
                    th ? "📋 บันทึกข้อมูลยา\nเวลา: %s\nโมดูล: %d (%s)\n"
                       : "📋 Medication setup saved\nTime: %s\nModule: %d (%s)\n",
                    time_str, med_idx + 1, disp_name);

                if (name_changed) {
                    const char *o = was->name[0] ? was->name : (th ? "(ว่าง)" : "(empty)");
                    const char *n = now->name[0] ? now->name : (th ? "(ว่าง)" : "(empty)");
                    off += snprintf(msg + off, sizeof(msg) - off,
                        th ? "• ชื่อยา: %s → %s\n" : "• Name: %s → %s\n", o, n);
                }
                if (count_changed) {
                    off += snprintf(msg + off, sizeof(msg) - off,
                        th ? "• จำนวน: %d → %d เม็ด (%+d)\n"
                           : "• Count: %d → %d pills (%+d)\n",
                        was->count, now->count, now->count - was->count);
                }
                if (slots_changed) {
                    const char *o = old_slots[0] ? old_slots : "-";
                    const char *n = new_slots[0] ? new_slots : "-";
                    off += snprintf(msg + off, sizeof(msg) - off,
                        th ? "• มื้อจ่าย: %s → %s\n" : "• Slots: %s → %s\n", o, n);
                }

                extern void dispenser_telegram_photo_msg(const char *msg);
                dispenser_telegram_photo_msg(msg);
            }
        }

        pending_page = PAGE_SETUP_MEDS;
        force_redraw = true;
        return;
    }

    if (show_return_confirm) {
        if (ty_n >= 60 && ty_n <= 310 && tx_n >= 20 && tx_n <= 460) {
            if (ty_n >= 140 && ty_n <= 190) { // Quantity modifiers
                if (tx_n >= RET_MINUS_X && tx_n <= RET_MINUS_X + RET_MINUS_W) {
                    if (current_stock <= 0) return_qty = 100;
                    else if (return_qty == 100) return_qty = current_stock; /* always > 0 here */
                    else if (return_qty > 1) return_qty--;            /* min 1 — never let it hit 0 */
                } else if (tx_n >= RET_PLUS_X && tx_n <= RET_PLUS_X + RET_PLUS_W) {
                    if (current_stock <= 0) return_qty = 100;
                    else if (return_qty == 100) return_qty = current_stock;
                    else if (return_qty < current_stock) return_qty++;
                } else if (tx_n >= RET_ALL_X && tx_n <= RET_ALL_X + RET_ALL_W) {
                    dfplayer_play_track(29); // Track 29 for 'ALL' (was 30, physically shifted by FAT)
                    return_qty = 100; // Eject All Flag
                }
            } else if (ty_n >= 200 && ty_n <= 240) { // Action Buttons
                if (tx_n >= 60 && tx_n <= 220) { // CANCEL
                    dfplayer_play_track(12); // Cancel voice is at FAT index 12
                    show_return_confirm = false;
                    force_redraw = true; // explicitly wipe screen
                } else if (tx_n >= 260 && tx_n <= 420) { // CONFIRM
                    /* Defensive clamp — return_qty should never be 0
                     * (the minus button clamps to 1 above) but if
                     * something else has left it zero, fix it now so
                     * we don't silently swallow the user's CONFIRM tap
                     * and leave them staring at a frozen screen. */
                    if (return_qty <= 0) return_qty = 1;
                    dfplayer_play_track(28); // Confirm sound (track 28)
                    if (return_qty == 100) {
                        dispenser_manual_dispense(med_idx, 100);
                    } else {
                        dispenser_manual_dispense(med_idx, return_qty);
                    }
                    show_return_confirm = false;
                    return_qty = 1;
                    force_redraw = true; // explicitly wipe screen
                }
            }
        }
    } else {
        const netpie_shadow_t *sh = netpie_get_shadow();
        
        /* Stock buttons expanded to 50×48 px (was 40×36) — hit zone
         * widened to match the new visual size for easier finger taps.
         * Name input + slot panel y-ranges below were untouched and the
         * count box still occupies the middle. */
        if (ty_n >= 60 && ty_n <= 108) { // Zone 1 (stock row)
            if (tx_n >= 252 && tx_n <= 302) { // [-] stock
                if (current_stock > 0) {
                    netpie_shadow_update_count(med_idx + 1, current_stock - 1);
                }
            } else if (tx_n >= 382 && tx_n <= 432) { // [+] stock
                /* User-configurable ceiling — set via NETPIE web widget,
                 * persisted to NVS through the shadow. Falls back to the
                 * compile-time DISPENSER_MAX_PILLS when shadow hasn't
                 * loaded or the saved value is invalid. */
                int max_pills = dispenser_max_pills();
                if (current_stock < max_pills) {
                    netpie_shadow_update_count(med_idx + 1, current_stock + 1);
                    /* User actually refilled — the "I'll refill" promise
                     * is fulfilled, drop the Back-block latch. */
                    if (current_stock + 1 > 0) s_refill_pending = false;
                }
            } else if (tx_n >= 10 && tx_n <= 250) { // Name input
                dfplayer_play_track(30); // Track 30 for Name Input (was 31)
                snprintf(kb_title_buf, sizeof(kb_title_buf), "Name for Module %d:", med_idx + 1);
                ui_utf8_safe_truncate_copy(kb_input_buf, sizeof(kb_input_buf), sh->med[med_idx].name);
                kb_input_dirty = true;
                edit_slot = med_idx; 
                is_med_name_setup = true;
                ui_keyboard_prepare(g_ui_language == UI_LANG_TH);
                pending_page = PAGE_KEYBOARD;
                force_redraw = true;
            }
        }
        else if (ty_n >= 114 && ty_n <= 230) { // Zone 2: slots
            int tapped_slot = -1;

            const int row_x = 16;
            const int row_h = 34;
            const int row_gap = 6;
            const int start_y = 114;
            const int label_w = 94;
            const int gap = 8;
            const int btn_w = 88;
            const int bed_x = 314;
            const int bed_w = 150;
            const int bed_h = (row_h * 3) + (row_gap * 2);

            for (int row = 0; row < 3 && tapped_slot < 0; ++row) {
                int row_y = start_y + (row * (row_h + row_gap));
                int before_x = row_x + label_w + gap;
                int after_x = before_x + btn_w + gap;
                if (ty_n >= row_y + 5 && ty_n <= row_y + 29) {
                    if (tx_n >= before_x && tx_n <= before_x + btn_w) tapped_slot = row * 2;
                    else if (tx_n >= after_x && tx_n <= after_x + btn_w) tapped_slot = (row * 2) + 1;
                }
            }

            if (tapped_slot < 0 &&
                tx_n >= bed_x && tx_n <= bed_x + bed_w &&
                ty_n >= start_y && ty_n <= start_y + bed_h) {
                tapped_slot = 6;
            }

            if (tapped_slot >= 0) {
                dfplayer_play_track(15 + tapped_slot); // Tracks 15-21 based on slot index
                uint8_t current_slots = sh->med[med_idx].slots;
                if (tapped_slot == 6) {
                    current_slots ^= (1 << tapped_slot);
                } else {
                    int pair_base = (tapped_slot / 2) * 2;
                    uint8_t pair_mask = (uint8_t)((1 << pair_base) | (1 << (pair_base + 1)));
                    uint8_t tapped_mask = (uint8_t)(1 << tapped_slot);

                    if (current_slots & tapped_mask) {
                        current_slots &= (uint8_t)~tapped_mask;
                    } else {
                        current_slots &= (uint8_t)~pair_mask;
                        current_slots |= tapped_mask;
                    }
                }
                netpie_shadow_update_med_slots(med_idx + 1, current_slots);
            }
        }
        else if (ty_n >= 240 && ty_n <= 290) { // Zone 3: Dispense Main Button
            if (tx_n >= 16 && tx_n <= 464) {
                if (!show_return_confirm && ui_manual_disp_status == 0) {
                    dfplayer_play_track(39); // Using dedicated track 39 for entering Dispense Mode instead of tracking 10
                    show_return_confirm = true;
                    return_qty = (current_stock > 0) ? 1 : 100;
                    force_redraw = true;
                }
            }
        }
    }
}

