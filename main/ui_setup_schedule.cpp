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
    for (int16_t row = 0; row < label->height; ++row) {
        const uint16_t *src = label->pixels + (row * label->width);
        int16_t run_start = -1;
        for (int16_t col = 0; col <= label->width; ++col) {
            bool opaque = false;
            if (col < label->width) {
                uint16_t px = src[col];
                opaque = (px != THEME_PANEL && px != THEME_BG && px != THEME_BAD &&
                          px != THEME_OK && px != THEME_INACTIVE);
            }
            if (opaque && run_start < 0) run_start = col;
            else if (!opaque && run_start >= 0) {
                ui_draw_rgb_bitmap(x + run_start, y + row, col - run_start, 1, src + run_start);
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
#define SCHED_TITLE_PAD_X   10
#define SCHED_TITLE_TOP_Y   62
#define SCHED_TITLE_BOT_Y   197
#define SCHED_ROW1_LABEL_Y  94
#define SCHED_ROW2_LABEL_Y  134
#define SCHED_ROW3_LABEL_Y  229
#define SCHED_ROW4_LABEL_Y  269

void ui_setup_schedule_render(void)
{
    if (force_redraw) {
        fill_screen(THEME_BG);
        draw_top_bar_with_back(g_ui_language == UI_LANG_TH ? NULL : "Schedule Setup");
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label((LCD_W - kThTopSchedule.width) / 2, 6, &kThTopSchedule);
        }

        bool enabled = netpie_get_shadow()->enabled;
        fill_rect(380, 8, 80, 28, enabled ? THEME_OK : THEME_INACTIVE);
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label(380 + ((80 - (enabled ? kThOn.width : kThOff.width)) / 2), 8, enabled ? &kThOn : &kThOff);
        } else {
            draw_string_gfx(enabled ? 400 : 395, 28, enabled ? "ON" : "OFF", 0xFFFF, enabled ? THEME_OK : THEME_INACTIVE, &FreeSans12pt7b);
        }

        fill_rect(SCHED_LEFT_X, SCHED_TOP_Y, SCHED_CARD_W, SCHED_TOP_H, 0xFFFF);
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label(SCHED_LEFT_X + SCHED_TITLE_PAD_X, SCHED_TITLE_TOP_Y, &kThMorning);
            draw_schedule_label(SCHED_LEFT_X + SCHED_TITLE_PAD_X, SCHED_ROW1_LABEL_Y, &kThBefore);
        } else {
            draw_string_gfx(20, 80, "Morning", 0x8430, 0xFFFF, &FreeSans12pt7b);
            draw_string_gfx(20, 110, "Before", THEME_INACTIVE, 0xFFFF, &FreeSans9pt7b);
        }
        fill_rect(110, 92, 115, 30, THEME_PANEL);
        draw_string_gfx(120, 114, netpie_get_shadow()->slot_time[0], 0x0000, THEME_PANEL, &FreeSans12pt7b);
        if (g_ui_language == UI_LANG_TH) draw_schedule_label(SCHED_LEFT_X + SCHED_TITLE_PAD_X, SCHED_ROW2_LABEL_Y, &kThAfter);
        else draw_string_gfx(20, 150, "After", THEME_INACTIVE, 0xFFFF, &FreeSans9pt7b);
        fill_rect(110, 132, 115, 30, THEME_PANEL);
        draw_string_gfx(120, 154, netpie_get_shadow()->slot_time[1], 0x0000, THEME_PANEL, &FreeSans12pt7b);

        fill_rect(SCHED_RIGHT_X, SCHED_TOP_Y, SCHED_CARD_W, SCHED_TOP_H, 0xFFFF);
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label(SCHED_RIGHT_X + SCHED_TITLE_PAD_X, SCHED_TITLE_TOP_Y, &kThNoon);
            draw_schedule_label(SCHED_RIGHT_X + SCHED_TITLE_PAD_X, SCHED_ROW1_LABEL_Y, &kThBefore);
        } else {
            draw_string_gfx(255, 80, "Noon", 0xC2A0, 0xFFFF, &FreeSans12pt7b);
            draw_string_gfx(255, 110, "Before", THEME_INACTIVE, 0xFFFF, &FreeSans9pt7b);
        }
        fill_rect(345, 92, 115, 30, THEME_PANEL);
        draw_string_gfx(355, 114, netpie_get_shadow()->slot_time[2], 0x0000, THEME_PANEL, &FreeSans12pt7b);
        if (g_ui_language == UI_LANG_TH) draw_schedule_label(SCHED_RIGHT_X + SCHED_TITLE_PAD_X, SCHED_ROW2_LABEL_Y, &kThAfter);
        else draw_string_gfx(255, 150, "After", THEME_INACTIVE, 0xFFFF, &FreeSans9pt7b);
        fill_rect(345, 132, 115, 30, THEME_PANEL);
        draw_string_gfx(355, 154, netpie_get_shadow()->slot_time[3], 0x0000, THEME_PANEL, &FreeSans12pt7b);

        fill_rect(SCHED_LEFT_X, SCHED_BOTTOM_Y, SCHED_CARD_W, SCHED_BOTTOM_H, 0xFFFF);
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label(SCHED_LEFT_X + SCHED_TITLE_PAD_X, SCHED_TITLE_BOT_Y, &kThEvening);
            draw_schedule_label(SCHED_LEFT_X + SCHED_TITLE_PAD_X, SCHED_ROW3_LABEL_Y, &kThBefore);
        } else {
            draw_string_gfx(20, 215, "Evening", 0x8210, 0xFFFF, &FreeSans12pt7b);
            draw_string_gfx(20, 245, "Before", THEME_INACTIVE, 0xFFFF, &FreeSans9pt7b);
        }
        fill_rect(110, 227, 115, 30, THEME_PANEL);
        draw_string_gfx(120, 249, netpie_get_shadow()->slot_time[4], 0x0000, THEME_PANEL, &FreeSans12pt7b);
        if (g_ui_language == UI_LANG_TH) draw_schedule_label(SCHED_LEFT_X + SCHED_TITLE_PAD_X, SCHED_ROW4_LABEL_Y, &kThAfter);
        else draw_string_gfx(20, 285, "After", THEME_INACTIVE, 0xFFFF, &FreeSans9pt7b);
        fill_rect(110, 267, 115, 30, THEME_PANEL);
        draw_string_gfx(120, 289, netpie_get_shadow()->slot_time[5], 0x0000, THEME_PANEL, &FreeSans12pt7b);

        fill_rect(SCHED_RIGHT_X, SCHED_BOTTOM_Y, SCHED_CARD_W, SCHED_BOTTOM_H, 0xFFFF);
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label(SCHED_RIGHT_X + SCHED_TITLE_PAD_X, SCHED_TITLE_BOT_Y, &kThBedtime);
        } else {
            draw_string_gfx(255, 215, "Bed", 0x1A1F, 0xFFFF, &FreeSans12pt7b);
            draw_string_gfx(255, 265, "Time", THEME_INACTIVE, 0xFFFF, &FreeSans9pt7b);
        }
        fill_rect(345, 247, 115, 30, THEME_PANEL);
        draw_string_gfx(355, 269, netpie_get_shadow()->slot_time[6], 0x0000, THEME_PANEL, &FreeSans12pt7b);
        
        force_redraw = false;
    } else {
        // Partial Update for Master Toggle
        static bool prev_enabled = false;
        bool enabled = netpie_get_shadow()->enabled;
        if (enabled != prev_enabled) {
            fill_rect(380, 8, 80, 28, enabled ? THEME_OK : THEME_INACTIVE);
            if (g_ui_language == UI_LANG_TH) {
                draw_schedule_label(380 + ((80 - (enabled ? kThOn.width : kThOff.width)) / 2), 8, enabled ? &kThOn : &kThOff);
            } else {
                draw_string_gfx(enabled ? 400 : 395, 28, enabled ? "ON" : "OFF", 0xFFFF, enabled ? THEME_OK : THEME_INACTIVE, &FreeSans12pt7b);
            }
            prev_enabled = enabled;
        }
    }
}

void ui_setup_schedule_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    if (ty_n < 44) {
        if (tx_n < 120) {
            dfplayer_play_track(10);
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

        // ── Column labels ──
        if (g_ui_language == UI_LANG_TH) {
            draw_schedule_label(115, 50, &kThHour);
            draw_schedule_label(284, 50, &kThMinute);
        } else {
            draw_string_centered(160, 66, "HOUR",   SB_COLOR_TXT_MUTED, SB_COLOR_BG, &FreeSans9pt7b);
            draw_string_centered(320, 66, "MINUTE", SB_COLOR_TXT_MUTED, SB_COLOR_BG, &FreeSans9pt7b);
        }

        // ── Plus buttons y=76 h=40 ──
        fill_rect(80,  76, 160, 40, SB_COLOR_CARD);
        draw_rect(80,  76, 160, 40, SB_COLOR_BORDER);
        // + icon at center (160, 96)
        fill_rect(150, 94, 20, 4, SB_COLOR_TXT_MAIN);
        fill_rect(158, 86, 4, 20, SB_COLOR_TXT_MAIN);

        fill_rect(240, 76, 160, 40, SB_COLOR_CARD);
        draw_rect(240, 76, 160, 40, SB_COLOR_BORDER);
        // + icon at center (320, 96)
        fill_rect(310, 94, 20, 4, SB_COLOR_TXT_MAIN);
        fill_rect(318, 86, 4, 20, SB_COLOR_TXT_MAIN);

        // ── Time display zone y=128..176 ──
        fill_rect(80, 128, 320, 48, SB_COLOR_BG);

        // ── Separator lines (drawn AFTER clearing time zone) ──
        fill_rect(40, 127, 400, 1, SB_COLOR_BORDER);
        fill_rect(40, 177, 400, 1, SB_COLOR_BORDER);

        // ── Minus buttons y=188 h=40 ──
        fill_rect(80,  188, 160, 40, THEME_BAD);
        draw_rect(80,  188, 160, 40, SB_COLOR_BORDER);
        // - icon at center (160, 208)
        fill_rect(150, 206, 20, 4, 0xFFFF);

        fill_rect(240, 188, 160, 40, THEME_BAD);
        draw_rect(240, 188, 160, 40, SB_COLOR_BORDER);
        // - icon at center (320, 208)
        fill_rect(310, 206, 20, 4, 0xFFFF);

        // ── Cancel / Save y=248 h=40 ──
        fill_rect(56,  248, 140, 40, THEME_BAD);
        draw_rect(56,  248, 140, 40, SB_COLOR_BORDER);
        if (g_ui_language == UI_LANG_TH) draw_schedule_label(56 + ((140 - kThCancelDark.width) / 2), 254, &kThCancelDark);
        else draw_string_centered(126, 274, "CANCEL", 0xFFFF, THEME_BAD, &FreeSans12pt7b);

        fill_rect(284, 248, 140, 40, THEME_OK);
        draw_rect(284, 248, 140, 40, SB_COLOR_BORDER);
        if (g_ui_language == UI_LANG_TH) draw_schedule_label(284 + ((140 - kThSaveDark.width) / 2), 254, &kThSaveDark);
        else draw_string_centered(354, 274, "SAVE",   0x0000, THEME_OK, &FreeSans12pt7b);
    }

    // ── Time digits — only redraw when value changes ──
    static int8_t tp_prev_hh = -1, tp_prev_mm = -1;
    if (force_redraw || edit_hh != tp_prev_hh || edit_mm != tp_prev_mm) {
        tp_prev_hh = edit_hh;
        tp_prev_mm = edit_mm;
        char buf[16];
        snprintf(buf, sizeof(buf), "  %02d : %02d  ", edit_hh, edit_mm);
        draw_string_centered(LCD_W / 2, 162, buf, SB_COLOR_PRIMARY, SB_COLOR_BG, &FreeSansBold24pt7b);
    }
}

void ui_time_picker_handle_touch(uint16_t tx_n, uint16_t ty_n, bool long_press)
{
    bool in_hour_plus  = (tx_n >= 80  && tx_n <= 240 && ty_n >= 76  && ty_n <= 116);
    bool in_min_plus   = (tx_n >= 240 && tx_n <= 400 && ty_n >= 76  && ty_n <= 116);
    bool in_hour_minus = (tx_n >= 80  && tx_n <= 240 && ty_n >= 188 && ty_n <= 228);
    bool in_min_minus  = (tx_n >= 240 && tx_n <= 400 && ty_n >= 188 && ty_n <= 228);

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
    bool in_hour_plus  = (tx_n >= 80  && tx_n <= 240 && ty_n >= 76  && ty_n <= 116);
    bool in_min_plus   = (tx_n >= 240 && tx_n <= 400 && ty_n >= 76  && ty_n <= 116);
    bool in_hour_minus = (tx_n >= 80  && tx_n <= 240 && ty_n >= 188 && ty_n <= 228);
    bool in_min_minus  = (tx_n >= 240 && tx_n <= 400 && ty_n >= 188 && ty_n <= 228);

    if (in_hour_plus)  edit_hh = (edit_hh + 1) % 24;
    if (in_min_plus)   edit_mm = (edit_mm + 1) % 60;
    if (in_hour_minus) edit_hh = (edit_hh + 23) % 24;
    if (in_min_minus)  edit_mm = (edit_mm + 59) % 60;
}
