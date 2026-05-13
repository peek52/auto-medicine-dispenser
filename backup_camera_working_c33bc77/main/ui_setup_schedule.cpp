#include "ui_core.h"
#include "netpie_mqtt.h"
#include "dfplayer.h"
#include "ui_schedule_thai_labels.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void draw_schedule_label(int16_t x, int16_t y, const ui_label_bitmap_t *label)
{
    if (!label || !label->pixels) return;
    uint16_t transparent = label->pixels[0];
    for (int16_t row = 0; row < label->height; ++row) {
        const uint16_t *src = label->pixels + (row * label->width);
        int16_t run_start = -1;
        for (int16_t col = 0; col <= label->width; ++col) {
            bool opaque = false;
            if (col < label->width) {
                uint16_t px = src[col];
                opaque = (px != transparent);
            }
            if (opaque && run_start < 0) run_start = col;
            else if (!opaque && run_start >= 0) {
                fill_rect(x + run_start, y + row, col - run_start, 1, 0xFFFF);
                run_start = -1;
            }
        }
    }
}

#define SCHED_LEFT_X        10
#define SCHED_RIGHT_X       245
#define SCHED_TOP_Y         55
#define SCHED_BOTTOM_Y      190
#define SCHED_CARD_W        225
#define SCHED_TOP_H         125
#define SCHED_BOTTOM_H      120
#define SCHED_TITLE_PAD_X   18
#define SCHED_TITLE_TOP_Y   68
#define SCHED_TITLE_BOT_Y   202
#define SCHED_ROW1_LABEL_Y  98
#define SCHED_ROW2_LABEL_Y  140
#define SCHED_ROW3_LABEL_Y  234
#define SCHED_ROW4_LABEL_Y  276

#define SCHED_SLOT_W        132
#define SCHED_SLOT_H        40
#define SCHED_SLOT_X_L      92
#define SCHED_SLOT_X_R      327

#define TP_BTN_W            160
#define TP_BTN_H            40

static const uint16_t SCHED_MORNING = ST_RGB565(245, 158, 11);
static const uint16_t SCHED_NOON    = ST_RGB565(14, 165, 233);
static const uint16_t SCHED_EVENING = ST_RGB565(99, 102, 241);
static const uint16_t SCHED_BED     = ST_RGB565(20, 184, 166);

#define TP_HOUR_BOX_X       78
#define TP_HOUR_BOX_W       124
#define TP_MIN_BOX_X        274
#define TP_MIN_BOX_W        124
#define TP_PLUS_BTN_Y       100
#define TP_VALUE_BOX_Y      150
#define TP_VALUE_BOX_H      50
#define TP_VALUE_BASELINE_Y 184
#define TP_MINUS_BTN_Y      206

static void draw_time_picker_value_box(int x, int w, int value)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d", value);
    fill_round_rect_frame(x, TP_VALUE_BOX_Y, w, TP_VALUE_BOX_H, 10, THEME_PANEL, SB_COLOR_BORDER);
    draw_string_centered(x + (w / 2), TP_VALUE_BASELINE_Y, buf, 0xFFFF, THEME_PANEL, &FreeSansBold18pt7b);
}

static void redraw_time_picker_value_only(int x, int w, int value)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d", value);
    fill_round_rect(x + 1, TP_VALUE_BOX_Y + 1, w - 2, TP_VALUE_BOX_H - 2, 9, THEME_PANEL);
    draw_string_centered(x + (w / 2), TP_VALUE_BASELINE_Y, buf, 0xFFFF, THEME_PANEL, &FreeSansBold18pt7b);
}

static void draw_schedule_card_shell(int x, int y, int w, int h, uint16_t accent)
{
    fill_round_rect_frame(x, y, w, h, 14, SB_COLOR_CARD, SB_COLOR_BORDER);
    fill_rect(x + 12, y + 20, 4, h - 40, accent);
}

static void draw_schedule_time_chip(int x, int y, const char *time, uint16_t accent)
{
    fill_round_rect_frame(x, y, SCHED_SLOT_W, SCHED_SLOT_H, 10, THEME_PANEL, THEME_BORDER);
    fill_rect(x + 10, y + 6, 6, SCHED_SLOT_H - 12, accent);
    draw_string_centered(x + (SCHED_SLOT_W / 2) + 8, y + 27, time, 0xFFFF, THEME_PANEL, &FreeSans12pt7b);
}

static void draw_time_picker_button(int x, int y, uint16_t fill, bool is_plus)
{
    fill_round_rect_frame(x, y, TP_BTN_W, TP_BTN_H, 10, fill, SB_COLOR_BORDER);
    fill_rect(x + (TP_BTN_W / 2) - 10, y + 18, 20, 4, 0xFFFF);
    if (is_plus) {
        fill_rect(x + (TP_BTN_W / 2) - 2, y + 10, 4, 20, 0xFFFF);
    }
}

void ui_setup_schedule_render(void)
{
    static bool prev_enabled = false;

    if (force_redraw) {
        fill_screen(THEME_BG);
        draw_top_bar_with_back(g_ui_language == UI_LANG_TH ? NULL : "Schedule Setup");
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label((LCD_W - kThTopSchedule.width) / 2, 6, &kThTopSchedule);
        }

        bool enabled = netpie_get_shadow()->enabled;
        prev_enabled = enabled;
        fill_round_rect(374, 8, 88, 28, 14, enabled ? THEME_OK : THEME_INACTIVE);
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label(374 + ((88 - (enabled ? kThOn.width : kThOff.width)) / 2), 8, enabled ? &kThOn : &kThOff);
        } else {
            draw_string_centered(418, 28, enabled ? "ON" : "OFF", 0xFFFF, enabled ? THEME_OK : THEME_INACTIVE, &FreeSans12pt7b);
        }

        draw_schedule_card_shell(SCHED_LEFT_X, SCHED_TOP_Y, SCHED_CARD_W, SCHED_TOP_H, SCHED_MORNING);
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label(SCHED_LEFT_X + SCHED_TITLE_PAD_X, SCHED_TITLE_TOP_Y, &kThMorning);
            draw_schedule_label(SCHED_LEFT_X + SCHED_TITLE_PAD_X, SCHED_ROW1_LABEL_Y, &kThBefore);
        } else {
            draw_string_gfx(24, 82, "Morning", 0xFFFF, SB_COLOR_CARD, &FreeSans12pt7b);
            draw_string_gfx(34, 110, "Before", THEME_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
        }
        draw_schedule_time_chip(SCHED_SLOT_X_L, 90, netpie_get_shadow()->slot_time[0], SCHED_MORNING);
        if (g_ui_language == UI_LANG_TH) draw_schedule_label(SCHED_LEFT_X + SCHED_TITLE_PAD_X, SCHED_ROW2_LABEL_Y, &kThAfter);
        else draw_string_gfx(34, 150, "After", THEME_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
        draw_schedule_time_chip(SCHED_SLOT_X_L, 130, netpie_get_shadow()->slot_time[1], SCHED_MORNING);

        draw_schedule_card_shell(SCHED_RIGHT_X, SCHED_TOP_Y, SCHED_CARD_W, SCHED_TOP_H, SCHED_NOON);
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label(SCHED_RIGHT_X + SCHED_TITLE_PAD_X, SCHED_TITLE_TOP_Y, &kThNoon);
            draw_schedule_label(SCHED_RIGHT_X + SCHED_TITLE_PAD_X, SCHED_ROW1_LABEL_Y, &kThBefore);
        } else {
            draw_string_gfx(259, 82, "Noon", 0xFFFF, SB_COLOR_CARD, &FreeSans12pt7b);
            draw_string_gfx(269, 110, "Before", THEME_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
        }
        draw_schedule_time_chip(SCHED_SLOT_X_R, 90, netpie_get_shadow()->slot_time[2], SCHED_NOON);
        if (g_ui_language == UI_LANG_TH) draw_schedule_label(SCHED_RIGHT_X + SCHED_TITLE_PAD_X, SCHED_ROW2_LABEL_Y, &kThAfter);
        else draw_string_gfx(269, 150, "After", THEME_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
        draw_schedule_time_chip(SCHED_SLOT_X_R, 130, netpie_get_shadow()->slot_time[3], SCHED_NOON);

        draw_schedule_card_shell(SCHED_LEFT_X, SCHED_BOTTOM_Y, SCHED_CARD_W, SCHED_BOTTOM_H, SCHED_EVENING);
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label(SCHED_LEFT_X + SCHED_TITLE_PAD_X, SCHED_TITLE_BOT_Y, &kThEvening);
            draw_schedule_label(SCHED_LEFT_X + SCHED_TITLE_PAD_X, SCHED_ROW3_LABEL_Y, &kThBefore);
        } else {
            draw_string_gfx(24, 217, "Evening", 0xFFFF, SB_COLOR_CARD, &FreeSans12pt7b);
            draw_string_gfx(34, 245, "Before", THEME_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
        }
        draw_schedule_time_chip(SCHED_SLOT_X_L, 225, netpie_get_shadow()->slot_time[4], SCHED_EVENING);
        if (g_ui_language == UI_LANG_TH) draw_schedule_label(SCHED_LEFT_X + SCHED_TITLE_PAD_X, SCHED_ROW4_LABEL_Y, &kThAfter);
        else draw_string_gfx(34, 285, "After", THEME_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
        draw_schedule_time_chip(SCHED_SLOT_X_L, 265, netpie_get_shadow()->slot_time[5], SCHED_EVENING);

        draw_schedule_card_shell(SCHED_RIGHT_X, SCHED_BOTTOM_Y, SCHED_CARD_W, SCHED_BOTTOM_H, SCHED_BED);
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label(SCHED_RIGHT_X + SCHED_TITLE_PAD_X, SCHED_TITLE_BOT_Y, &kThBedtime);
        } else {
            draw_string_gfx(259, 217, "Bedtime", 0xFFFF, SB_COLOR_CARD, &FreeSans12pt7b);
            draw_string_gfx(269, 245, "Night", THEME_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
        }
        draw_schedule_time_chip(SCHED_SLOT_X_R, 247, netpie_get_shadow()->slot_time[6], SCHED_BED);
        
        force_redraw = false;
    } else {
        // Partial Update for Master Toggle
        bool enabled = netpie_get_shadow()->enabled;
        if (enabled != prev_enabled) {
            fill_round_rect(374, 8, 88, 28, 14, enabled ? THEME_OK : THEME_INACTIVE);
            if (g_ui_language == UI_LANG_TH) {
                draw_schedule_label(374 + ((88 - (enabled ? kThOn.width : kThOff.width)) / 2), 8, enabled ? &kThOn : &kThOff);
            } else {
                draw_string_centered(418, 28, enabled ? "ON" : "OFF", 0xFFFF, enabled ? THEME_OK : THEME_INACTIVE, &FreeSans12pt7b);
            }
            prev_enabled = enabled;
        }
    }
}

void ui_setup_schedule_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    if (ty_n < 44) {
        if (tx_n >= 14 && tx_n <= 118 && ty_n >= 8 && ty_n <= 34) {
            dfplayer_play_track(g_snd_button);
            pending_page = PAGE_MENU;
            edit_slot = -1;
        }
        else if (tx_n > 350) {
            bool en = netpie_get_shadow()->enabled;
            if (!en) {
                dfplayer_play_track(11); // Turning ON
            } else {
                dfplayer_play_track(13); // Turning OFF (user's FAT table assigned 13 to OFF voice)
            }
            netpie_shadow_update_enabled(!en);
            // Relies on partial UI update in render block
        }
    } else {
        if (tx_n >= 10 && tx_n <= 235) { // Left column (Morning/Evening)
            if (ty_n >= 55 && ty_n <= 118) edit_slot = 0;      
            else if (ty_n >= 119 && ty_n <= 180) edit_slot = 1; 
            else if (ty_n >= 190 && ty_n <= 250) edit_slot = 4;
            else if (ty_n >= 251 && ty_n <= 310) edit_slot = 5;
        } else if (tx_n >= 245 && tx_n <= 470) { // Right column (Noon/Bedtime)
            if (ty_n >= 55 && ty_n <= 118) edit_slot = 2;      
            else if (ty_n >= 119 && ty_n <= 180) edit_slot = 3; 
            else if (ty_n >= 190 && ty_n <= 310) edit_slot = 6; 
        }

        if (edit_slot >= 0) {
            dfplayer_play_track(15 + edit_slot); // Tracks 15-21 based on slot index
            const char *curr = netpie_get_shadow()->slot_time[edit_slot];
            int h = 0, m = 0;
            sscanf(curr, "%d:%d", &h, &m);
            edit_hh = h;
            edit_mm = m;
            pending_page = PAGE_TIME_PICKER;
            force_redraw = true;
        }
    }
}

void ui_time_picker_render(void)
{
    if (force_redraw) {
        // ── Complete clean clear first ──
        fill_screen(THEME_BG);

        // ── Top bar ──
        fill_rect(0, 0, LCD_W, 44, THEME_PANEL);
        fill_rect(0, 42, LCD_W, 2, THEME_ACCENT);
        if (g_ui_language == UI_LANG_TH) draw_schedule_label((LCD_W - kThSetTime.width) / 2, 6, &kThSetTime);
        else draw_string_centered(LCD_W / 2, 29, "Set Time", THEME_TXT_MAIN, THEME_PANEL, &FreeSans18pt7b);

        fill_round_rect_frame(28, 56, 424, 240, 18, SB_COLOR_CARD, SB_COLOR_BORDER);

        // ── Column labels ──
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label(115, 72, &kThHour);
            draw_schedule_label(284, 72, &kThMinute);
        } else {
            draw_string_centered(160, 72, "HOUR",   SB_COLOR_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
            draw_string_centered(320, 72, "MINUTE", SB_COLOR_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
        }

        // ── Plus buttons y=76 h=40 ──
        draw_time_picker_button(80, TP_PLUS_BTN_Y, THEME_OK, true);
        draw_time_picker_button(240, TP_PLUS_BTN_Y, THEME_OK, true);

        // ── Time display zone — covers value box area ──
        fill_round_rect_frame(64, 140, 352, 66, 16, SB_COLOR_BG, THEME_BORDER);

        // ── Separator lines (drawn AFTER clearing time zone) ──
        draw_string_centered(LCD_W / 2, TP_VALUE_BASELINE_Y, ":", SB_COLOR_PRIMARY, SB_COLOR_BG, &FreeSansBold24pt7b);

        // ── Minus buttons y=188 h=40 ──
        draw_time_picker_button(80, TP_MINUS_BTN_Y, THEME_BAD, false);
        draw_time_picker_button(240, TP_MINUS_BTN_Y, THEME_BAD, false);

        // ── Cancel / Save y=248 h=40 ──
        fill_round_rect_frame(56, 248, 140, 40, 12, THEME_BAD, SB_COLOR_BORDER);
        if (g_ui_language == UI_LANG_TH) draw_schedule_label(56 + ((140 - kThCancelDark.width) / 2), 254, &kThCancelDark);
        else draw_string_centered(126, 274, "CANCEL", 0xFFFF, THEME_BAD, &FreeSans12pt7b);

        fill_round_rect_frame(284, 248, 140, 40, 12, THEME_OK, SB_COLOR_BORDER);
        if (g_ui_language == UI_LANG_TH) draw_schedule_label(284 + ((140 - kThSaveDark.width) / 2), 254, &kThSaveDark);
        else draw_string_centered(354, 274, "SAVE",   0x0000, THEME_OK, &FreeSans12pt7b);
    }

    // ── Time digits — only redraw when value changes ──
    static int8_t tp_prev_hh = -1, tp_prev_mm = -1;
    if (force_redraw || edit_hh != tp_prev_hh || edit_mm != tp_prev_mm) {
        if (force_redraw || edit_hh != tp_prev_hh) {
            if (force_redraw) draw_time_picker_value_box(TP_HOUR_BOX_X, TP_HOUR_BOX_W, edit_hh);
            else redraw_time_picker_value_only(TP_HOUR_BOX_X, TP_HOUR_BOX_W, edit_hh);
            tp_prev_hh = edit_hh;
        }
        if (force_redraw || edit_mm != tp_prev_mm) {
            if (force_redraw) draw_time_picker_value_box(TP_MIN_BOX_X, TP_MIN_BOX_W, edit_mm);
            else redraw_time_picker_value_only(TP_MIN_BOX_X, TP_MIN_BOX_W, edit_mm);
            tp_prev_mm = edit_mm;
        }
    }
}

void ui_time_picker_handle_touch(uint16_t tx_n, uint16_t ty_n, bool long_press)
{
    bool in_hour_plus  = (tx_n >= 80  && tx_n <= 240 && ty_n >= TP_PLUS_BTN_Y  && ty_n <= (TP_PLUS_BTN_Y + 40));
    bool in_min_plus   = (tx_n >= 240 && tx_n <= 400 && ty_n >= TP_PLUS_BTN_Y  && ty_n <= (TP_PLUS_BTN_Y + 40));
    bool in_hour_minus = (tx_n >= 80  && tx_n <= 240 && ty_n >= TP_MINUS_BTN_Y && ty_n <= (TP_MINUS_BTN_Y + 40));
    bool in_min_minus  = (tx_n >= 240 && tx_n <= 400 && ty_n >= TP_MINUS_BTN_Y && ty_n <= (TP_MINUS_BTN_Y + 40));

    if (!long_press) {
        if (in_hour_plus)  edit_hh = (edit_hh + 1) % 24;
        if (in_min_plus)   edit_mm = (edit_mm + 1) % 60;
        if (in_hour_minus) edit_hh = (edit_hh + 23) % 24;
        if (in_min_minus)  edit_mm = (edit_mm + 59) % 60;
    }

    if (in_hour_plus || in_min_plus || in_hour_minus || in_min_minus) {
        return;
    }

    if (tx_n >= 56 && tx_n <= 196 && ty_n >= 248 && ty_n <= 288) {
        dfplayer_play_track(12); // Cancel (user's FAT table assigned 12 to Cancel)
        pending_page = PAGE_SETUP_SCHEDULE;
        edit_slot = -1;
    }
    else if (tx_n >= 284 && tx_n <= 424 && ty_n >= 248 && ty_n <= 288) {
        dfplayer_play_track(14); // Save (user's FAT table assigned 14 to Save voice)
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", edit_hh, edit_mm);
        netpie_shadow_update_slot(edit_slot, buf);
        pending_page = PAGE_SETUP_SCHEDULE;
        edit_slot = -1;
        force_redraw = true;
    }
}

void ui_time_picker_handle_hold(uint16_t tx_n, uint16_t ty_n)
{
    bool in_hour_plus  = (tx_n >= 80  && tx_n <= 240 && ty_n >= TP_PLUS_BTN_Y  && ty_n <= (TP_PLUS_BTN_Y + 40));
    bool in_min_plus   = (tx_n >= 240 && tx_n <= 400 && ty_n >= TP_PLUS_BTN_Y  && ty_n <= (TP_PLUS_BTN_Y + 40));
    bool in_hour_minus = (tx_n >= 80  && tx_n <= 240 && ty_n >= TP_MINUS_BTN_Y && ty_n <= (TP_MINUS_BTN_Y + 40));
    bool in_min_minus  = (tx_n >= 240 && tx_n <= 400 && ty_n >= TP_MINUS_BTN_Y && ty_n <= (TP_MINUS_BTN_Y + 40));

    if (in_hour_plus)  edit_hh = (edit_hh + 1) % 24;
    if (in_min_plus)   edit_mm = (edit_mm + 1) % 60;
    if (in_hour_minus) edit_hh = (edit_hh + 23) % 24;
    if (in_min_minus)  edit_mm = (edit_mm + 59) % 60;
}
