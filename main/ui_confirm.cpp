#include "ui_core.h"
#include "dispenser_scheduler.h"
#include "ui_confirm_thai_labels.h"
#include <stdio.h>

static void draw_confirm_label(int16_t x, int16_t y, const ui_label_bitmap_t *label)
{
    if (!label || !label->pixels) return;
    for (int16_t row = 0; row < label->height; ++row) {
        const uint16_t *src = label->pixels + (row * label->width);
        int16_t run_start = -1;
        for (int16_t col = 0; col <= label->width; ++col) {
            bool opaque = false;
            if (col < label->width) {
                uint16_t px = src[col];
                opaque = (px != SB_COLOR_CARD && px != THEME_BORDER && px != THEME_OK && px != THEME_BAD);
            }
            if (opaque && run_start < 0) run_start = col;
            else if (!opaque && run_start >= 0) {
                ui_draw_rgb_bitmap(x + run_start, y + row, col - run_start, 1, src + run_start);
                run_start = -1;
            }
        }
    }
}

void ui_confirm_render(void)
{
    static int last_sec = -1;
    int sec_left = dispenser_seconds_left();
    if (force_redraw || sec_left != last_sec) {
        last_sec = sec_left;
        if (force_redraw) {
            fill_screen(THEME_BAD); // Danger Red
            fill_round_rect_frame(10, 10, 460, 300, 16, SB_COLOR_CARD, SB_COLOR_TXT_MAIN);
            
            if (dispenser_is_empty_warning()) {
                if (g_ui_language == UI_LANG_TH) {
                    draw_confirm_label((LCD_W - kThOutOfStock.width) / 2, 28, &kThOutOfStock);
                } else {
                    draw_string_centered(LCD_W/2, 50, "OUT OF STOCK!", THEME_WARN, SB_COLOR_CARD, &FreeSansBold18pt7b);
                }
                
                fill_round_rect_frame(30, 120, 420, 150, 16, THEME_BORDER, 0x000F);
                if (g_ui_language == UI_LANG_TH) {
                    draw_confirm_label((LCD_W - kThPleaseRefill.width) / 2, 173, &kThPleaseRefill);
                    draw_confirm_label((LCD_W - kThTapDismiss.width) / 2, 228, &kThTapDismiss);
                } else {
                    draw_string_centered(LCD_W/2, 195, "PLEASE REFILL", 0x0000, THEME_BORDER, &FreeSansBold24pt7b);
                    draw_string_centered(LCD_W/2, 245, "TAP ANYWHERE TO DISMISS", 0x0000, THEME_BORDER, &FreeSans12pt7b);
                }
            } else {
                if (g_ui_language == UI_LANG_TH) {
                    draw_confirm_label((LCD_W - kThTimeToTake.width) / 2, 28, &kThTimeToTake);
                } else {
                    draw_string_centered(LCD_W/2, 50, "TIME TO TAKE MEDS!", THEME_BAD, SB_COLOR_CARD, &FreeSansBold18pt7b);
                }
                
                fill_round_rect_frame(30, 120, 420, 150, 16, THEME_OK, 0xFFFF);
                if (g_ui_language == UI_LANG_TH) {
                    draw_confirm_label((LCD_W - kThTapAnywhere.width) / 2, 173, &kThTapAnywhere);
                    draw_confirm_label((LCD_W - kThToDispense.width) / 2, 228, &kThToDispense);
                } else {
                    draw_string_centered(LCD_W/2, 195, "TAP ANYWHERE", 0xFFFF, THEME_OK, &FreeSansBold24pt7b);
                    draw_string_centered(LCD_W/2, 245, "TO DISPENSE", 0xFFFF, THEME_OK, &FreeSansBold18pt7b);
                }
            }
        }
        
        char time_str[32];
        snprintf(time_str, sizeof(time_str), "Auto-skip in %02d:%02d", sec_left / 60, sec_left % 60);
        fill_rect(60, 65, 360, 30, SB_COLOR_CARD);
        draw_string_centered(LCD_W/2, 85, time_str, THEME_BAD, SB_COLOR_CARD, &FreeSans12pt7b);
    }
}

void ui_confirm_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    // Let the user confirm from nearly anywhere on screen.
    // Keep only a tiny edge guard to ignore phantom touches around [0,0].
    if (tx_n < 8 || tx_n > (LCD_W - 8) || ty_n < 8 || ty_n > (LCD_H - 8)) {
        return;
    }

    if (dispenser_is_empty_warning()) {
        dispenser_skip_meds();
    } else {
        dispenser_confirm_meds();
    }
}
