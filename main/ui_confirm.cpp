#include "ui_core.h"
#include "dispenser_scheduler.h"
#include "ui_confirm_thai_labels.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* BUG FIX #6: page-enter tick used as input-debounce. A finger held or
 * released across the STANDBY→CONFIRM page transition would otherwise
 * dispatch the carried-over touch event into ui_confirm_handle_touch and
 * auto-confirm/auto-skip the dose. By recording the tick at page entry
 * and ignoring touches within IGNORE_AFTER_ENTER_MS, we force the user
 * to perform a fresh tap on the new page. */
static volatile uint32_t s_page_enter_tick = 0;
#define UI_CONFIRM_IGNORE_AFTER_ENTER_MS 350

extern "C" void ui_confirm_arm_on_enter(void)
{
    s_page_enter_tick = (uint32_t)xTaskGetTickCount();
}

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
                    draw_string_centered(LCD_W/2, 192, "REFILL NEEDED", 0x0000, THEME_BORDER, &FreeSansBold18pt7b);
                    draw_string_centered(LCD_W/2, 241, "TAP TO CLOSE", 0x0000, THEME_BORDER, &FreeSans12pt7b);
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
    /* BUG FIX #6: reject touch events that arrive too soon after the
     * page transitioned to PAGE_CONFIRM_MEDS. The previous behaviour
     * accepted the very first touch event the dispatcher delivered —
     * which was usually the release-edge of the tap that triggered the
     * page change in the first place, or a hold-carryover from STANDBY,
     * resulting in instant auto-confirm/auto-skip with no user intent.
     * 350 ms is short enough that a deliberate user tap on the new page
     * is not blocked, but long enough to swallow the carryover event. */
    uint32_t now_tick = (uint32_t)xTaskGetTickCount();
    uint32_t since_enter_ms = (now_tick - s_page_enter_tick) * portTICK_PERIOD_MS;
    if (s_page_enter_tick != 0 && since_enter_ms < UI_CONFIRM_IGNORE_AFTER_ENTER_MS) {
        return;
    }

    // Let the user confirm from nearly anywhere on screen.
    // Keep only a tiny edge guard to ignore phantom touches around [0,0].
    if (tx_n < 8 || tx_n > (LCD_W - 8) || ty_n < 8 || ty_n > (LCD_H - 8)) {
        return;
    }

    if (dispenser_is_empty_warning()) {
        dispenser_skip_meds();
        return;
    }

    /* Just signal the scheduler. The display task transitions back to
     * PAGE_STANDBY (dispenser_is_waiting() goes false → see display_clock
     * page-switch block) and standby's popup state 9 paints the
     * "กำลังจ่ายยา" overlay on the standby background. execute_dispense
     * waits for g_ui_dispensing_popup_painted before spinning the servo. */
    dispenser_confirm_meds();
}
