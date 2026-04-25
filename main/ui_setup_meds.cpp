#include "ui_core.h"
#include "netpie_mqtt.h"
#include "dispenser_scheduler.h"
#include "dfplayer.h"
#include "ui_meds_thai_labels.h"
#include "ui_return_thai_labels.h"
#include "ui_utf8_text.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pill_sensor_status.h"

static uint32_t s_blink_tick   = 0;
static bool     s_blink_phase  = false;
static bool     s_validation_popup = false;
static netpie_med_t s_med_backup = {0};
static bool     s_med_snapshot_saved = false;

extern volatile int ui_manual_disp_status;

// Suppress VL53 sensor → shadow auto-sync while the user is editing a med
// row. Without this the sensor sync overwrites every +/- tap before the
// user can confirm, so the count value can never be set manually.
extern "C" bool ui_meds_edit_in_progress(void)
{
    return current_page == PAGE_SETUP_MEDS_DETAIL ||
           current_page == PAGE_SETUP_MEDS ||
           current_page == PAGE_KEYBOARD;
}

// â”€â”€ File-scope modal timers (file-local only) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static uint32_t s_disp_done_tick = 0;
static uint32_t s_disp_fail_tick = 0;
static int      prev_return_qty  = 1;
static bool     prev_show_confirm = false;

#define MED_COUNT_BOX_X      302
#define MED_COUNT_BOX_Y      52
#define MED_COUNT_BOX_W      78
#define MED_COUNT_BOX_H      36
#define MED_COUNT_BOX_CX     (MED_COUNT_BOX_X + (MED_COUNT_BOX_W / 2))
#define MED_COUNT_BOX_TEXT_Y 74

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

    draw_slot_choice_button(before_x, btn_y, btn_w, btn_h, before_active, before_active ? s_blink_phase : true, "Before", "ก่อน");
    draw_slot_choice_button(after_x,  btn_y, btn_w, btn_h, after_active,  after_active  ? s_blink_phase : true, "After",  "หลัง");
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
    const int16_t start_y = 100;
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
        if (g_ui_language == UI_LANG_TH) draw_meds_label((LCD_W - kThTopMedSetup.width) / 2, 8, &kThTopMedSetup);
        else draw_string_centered(LCD_W / 2, 28, "Medicine Setup", THEME_TXT_MAIN, THEME_PANEL, &FreeSans12pt7b);

        const netpie_shadow_t *sh = netpie_get_shadow();

        const int card_w = 214;
        const int card_h = 78;
        const int left_x = 16;
        const int right_x = 250;
        const int start_y = 54;
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
}

void ui_setup_meds_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    if (ty_n < 50) {
        if (tx_n >= 14 && tx_n <= 118 && ty_n >= 8 && ty_n <= 34) {
            dfplayer_play_track(g_snd_button);
            pending_page = PAGE_MENU;
        }
    } else {
        const int card_w = 214;
        const int card_h = 78;
        const int left_x = 16;
        const int right_x = 250;
        const int start_y = 54;
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
                break;
            }
        }
    }
}

void ui_setup_meds_detail_render(void)
{
    static netpie_shadow_t tp_prev_sh = {0};
    static int prev_disp_status = 0;

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
        // Save snapshot of current med data ONLY on first entry (not on every force_redraw)
        if (!s_med_snapshot_saved) {
            const netpie_shadow_t *sh_snap = netpie_get_shadow();
            s_med_backup = sh_snap->med[selected_med_idx];
            s_med_snapshot_saved = true;
        }

        fill_screen(THEME_BG);
        draw_top_bar_with_back(NULL);
        char page_title[32];
        snprintf(page_title, sizeof(page_title), "Module %d Setup", selected_med_idx + 1);
        if (g_ui_language == UI_LANG_TH) draw_meds_label((LCD_W / 2) - (med_module_setup_label(selected_med_idx)->width / 2), 8, med_module_setup_label(selected_med_idx));
        else draw_string_centered(LCD_W / 2, 28, page_title, THEME_TXT_MAIN, THEME_PANEL, &FreeSans12pt7b);

        // SAVE button top-right
        fill_round_rect_frame(330, 6, 140, 34, 8, THEME_OK, THEME_OK);
        if (g_ui_language == UI_LANG_TH) {
            int16_t save_tw = ui_utf8_text_width("บันทึก");
            ui_utf8_draw_text(400 - (save_tw / 2), 13, "บันทึก", 0xFFFF);
        } else {
            draw_string_centered(400, 29, "SAVE", 0xFFFF, THEME_OK, &FreeSans12pt7b);
        }

        const netpie_shadow_t *sh = netpie_get_shadow();
        int med_idx = selected_med_idx;
        
        fill_round_rect_frame(10, 52, 240, 36, 6, SB_COLOR_CARD, SB_COLOR_BORDER);
        bool has_name = strlen(sh->med[med_idx].name) > 0;
        if (g_ui_language == UI_LANG_TH && !has_name) {
            draw_meds_label(16, 60, &kThTapSetName);
        } else {
            if (has_name) draw_med_name_line(16, 57, sh->med[med_idx].name, SB_COLOR_PRIMARY, SB_COLOR_CARD, false);
            else draw_string_gfx(16, 76, "Tap to set name...", SB_COLOR_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
        }

        fill_round_rect_frame(260, 52, 40, 36, 6, THEME_BAD, SB_COLOR_BORDER);
        draw_string_centered(280, 70, "-", 0xFFFF, THEME_BAD, &FreeSans18pt7b);
        
        draw_med_count_value(sh->med[med_idx].count);

        fill_round_rect_frame(380, 52, 40, 36, 6, THEME_OK, SB_COLOR_BORDER);
        draw_string_centered(400, 70, "+", 0xFFFF, THEME_OK, &FreeSans18pt7b);

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

            if (sh->med[med_idx].slots != tp_prev_sh.med[med_idx].slots) {
                fill_rect(10, 96, 458, 122, THEME_BG);
                draw_slot_selector_panel(sh->med[med_idx].slots);
                tp_prev_sh.med[med_idx].slots = sh->med[med_idx].slots;
            }
        } // Closed 'else' logic for Background Local Partial Updates!

        if (ui_manual_disp_status != prev_disp_status || (!force_redraw && ui_manual_disp_status > 0)) {
            bool is_new_status = (ui_manual_disp_status != prev_disp_status);
            prev_disp_status = ui_manual_disp_status;

            if (ui_manual_disp_status == 1) {
                fill_round_rect_frame(80, 100, 320, 120, 12, THEME_PANEL, THEME_BORDER);
                if (g_ui_language == UI_LANG_TH) draw_meds_label((LCD_W - kThDispensing.width) / 2, 148, &kThDispensing);
                else draw_string_centered(LCD_W/2, 165, "Dispensing...", THEME_TXT_MAIN, THEME_PANEL, &FreeSans18pt7b);
            } else if (ui_manual_disp_status == 2) {
                fill_round_rect_frame(80, 100, 320, 120, 12, THEME_OK, THEME_BORDER);
                if (g_ui_language == UI_LANG_TH) {
                    draw_meds_label((LCD_W - kThDispenseOk1.width) / 2, 132, &kThDispenseOk1);
                    draw_meds_label((LCD_W - kThDispenseOk2.width) / 2, 175, &kThDispenseOk2);
                } else {
                    draw_string_centered(LCD_W/2, 150, "Dispense Complete", 0xFFFF, THEME_OK, &FreeSans18pt7b);
                    draw_string_centered(LCD_W/2, 190, "Finished", 0xFFFF, THEME_OK, &FreeSans12pt7b);
                }
                if (is_new_status) s_disp_done_tick = xTaskGetTickCount();
            } else if (ui_manual_disp_status == 3) {
                fill_round_rect_frame(80, 100, 320, 120, 12, THEME_BAD, THEME_BORDER);
                if (g_ui_language == UI_LANG_TH) {
                    draw_meds_label((LCD_W - kThDispenseFail1.width) / 2, 132, &kThDispenseFail1);
                    draw_meds_label((LCD_W - kThDispenseFail2.width) / 2, 175, &kThDispenseFail2);
                } else {
                    draw_string_centered(LCD_W/2, 150, "Dispense Failed", 0xFFFF, THEME_BAD, &FreeSans18pt7b);
                    draw_string_centered(LCD_W/2, 190, "Hardware I2C disconnect", 0xFFFF, THEME_BAD, &FreeSans12pt7b);
                }
                if (is_new_status) s_disp_fail_tick = xTaskGetTickCount();
            } else if (ui_manual_disp_status == 0) {
                force_redraw = true; // Dismiss popup by full wipe
            }
        }
    }

    // Blink redraw removed — repainting the slot panel every 400ms caused
    // a perceptible whole-area flash whenever any value (count, name) was
    // changing concurrently, since the partial redraws layered. Slots are
    // already drawn with their selected-state colors at force_redraw and on
    // each slot/count/name change above; no animation is needed.
    (void)s_blink_phase; (void)s_blink_tick;

    // ── Validation popup overlay (REMOVED per user request) ──
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

    if (show_return_confirm) {
        // ... handled below
    } else if (tx_n >= 14 && tx_n <= 118 && ty_n >= 8 && ty_n <= 34) {

        // Back: revert ALL changes to snapshot before leaving
        const netpie_shadow_t *sh_now = netpie_get_shadow();
        const netpie_med_t *cur = &sh_now->med[med_idx];
        if (strcmp(cur->name, s_med_backup.name) != 0)
            netpie_shadow_update_med_name(med_idx + 1, s_med_backup.name);
        if (cur->count != s_med_backup.count)
            netpie_shadow_update_count(med_idx + 1, s_med_backup.count);
        if (cur->slots != s_med_backup.slots)
            netpie_shadow_update_med_slots(med_idx + 1, s_med_backup.slots);
        dfplayer_play_track(g_snd_button);
        s_validation_popup = false;
        s_med_snapshot_saved = false; // Reset for next entry
        pending_page = PAGE_SETUP_MEDS;
        force_redraw = true;
        return;
    } else if (ty_n >= 6 && ty_n <= 40 && tx_n >= 330 && tx_n <= 470) {
        // SAVE button: validate before saving
        const netpie_shadow_t *sh_check = netpie_get_shadow();
        bool has_name  = (sh_check->med[med_idx].name[0] != '\0');
        bool has_count = (sh_check->med[med_idx].count > 0);
        bool has_slots = (sh_check->med[med_idx].slots != 0);
        if (!has_name) {
            // If name is cleared, automatically reset count and slots to 0
            netpie_shadow_update_count(med_idx + 1, 0);
            netpie_shadow_update_med_slots(med_idx + 1, 0);
        } else if (!has_count || !has_slots) {
            return; // Ignore tap if incomplete but has a name
        }
        dfplayer_play_track(14);
        s_validation_popup = false;
        s_med_snapshot_saved = false; // Reset for next entry
        pending_page = PAGE_SETUP_MEDS;
        force_redraw = true;
        return;
    }

    if (show_return_confirm) {
        if (ty_n >= 60 && ty_n <= 310 && tx_n >= 20 && tx_n <= 460) {
            if (ty_n >= 140 && ty_n <= 190) { // Quantity modifiers
                if (tx_n >= RET_MINUS_X && tx_n <= RET_MINUS_X + RET_MINUS_W) { 
                    if (current_stock <= 0) return_qty = 100;
                    else if (return_qty == 100) return_qty = current_stock;
                    else if (return_qty > 0) return_qty--;
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
                    dfplayer_play_track(28); // Confirm sound (track 28)
                    if (return_qty == 100) {
                        dispenser_manual_dispense(med_idx, 100);
                    } else if (return_qty > 0) {
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
        
        if (ty_n >= 52 && ty_n <= 88) { // Zone 1
            if (tx_n >= 260 && tx_n <= 300) { // [-] stock
                if (current_stock > 0) {
                    netpie_shadow_update_count(med_idx + 1, current_stock - 1);
                    dispenser_audit_stock_adjust(med_idx, current_stock, current_stock - 1);
                }
            } else if (tx_n >= 380 && tx_n <= 420) { // [+] stock
                int max_pills = DISPENSER_MAX_PILLS;
                const pill_sensor_status_t *sns = pill_sensor_status_get(med_idx);
                if (sns && sns->max_pills > 0) max_pills = sns->max_pills;
                if (current_stock < max_pills) {
                    netpie_shadow_update_count(med_idx + 1, current_stock + 1);
                    dispenser_audit_stock_adjust(med_idx, current_stock, current_stock + 1);
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
        else if (ty_n >= 100 && ty_n <= 216) { // Zone 2: slots
            int tapped_slot = -1;

            const int row_x = 16;
            const int row_h = 34;
            const int row_gap = 6;
            const int start_y = 100;
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

