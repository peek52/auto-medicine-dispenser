#include "ui_core.h"
#include "dfplayer.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_settings_thai_labels.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_settings";

int g_alert_volume       = 25;
int g_nav_volume         = 15;
bool g_nav_sound_enabled = true;
enum ui_language_t g_ui_language = UI_LANG_EN;

#define CARD_X         22
#define CARD_W         436
#define CARD_INSET_X   8
#define CARD_RIGHT_PAD 12

static void draw_label_bitmap(int16_t x, int16_t y, const ui_label_bitmap_t *label)
{
    if (!label || !label->pixels) return;
    for (int16_t row = 0; row < label->height; ++row) {
        const uint16_t *src = label->pixels + (row * label->width);
        int16_t run_start = -1;

        for (int16_t col = 0; col <= label->width; ++col) {
            bool opaque = false;
            if (col < label->width) {
                opaque = (src[col] != SB_COLOR_CARD &&
                          src[col] != THEME_PANEL &&
                          src[col] != THEME_BAD &&
                          src[col] != THEME_OK &&
                          src[col] != SB_COLOR_PRIMARY &&
                          src[col] != THEME_BORDER);
            }

            if (opaque && run_start < 0) {
                run_start = col;
            } else if (!opaque && run_start >= 0) {
                ui_draw_rgb_bitmap(x + run_start, y + row, col - run_start, 1, src + run_start);
                run_start = -1;
            }
        }
    }
}

void settings_save_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i16(h, "vol_alt",   (int16_t)g_alert_volume);
        nvs_set_i16(h, "vol_nav",   (int16_t)g_nav_volume);
        nvs_set_u8 (h, "en_nav",    (uint8_t)g_nav_sound_enabled);
        nvs_set_u8 (h, "lang_ui",   (uint8_t)g_ui_language);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Settings saved: alt=%d nav=%d en=%d lang=%d", g_alert_volume, g_nav_volume, g_nav_sound_enabled, g_ui_language);
    }
}

void settings_load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READONLY, &h) == ESP_OK) {
        int16_t va = g_alert_volume;
        int16_t vn = g_nav_volume;
        uint8_t en = g_nav_sound_enabled;
        uint8_t lang = (uint8_t)g_ui_language;
        
        nvs_get_i16(h, "vol_alt", &va);
        nvs_get_i16(h, "vol_nav", &vn);
        if (nvs_get_u8 (h, "en_nav",  &en) == ESP_OK) {
            g_nav_sound_enabled = (en > 0);
        }
        if (nvs_get_u8(h, "lang_ui", &lang) == ESP_OK) {
            g_ui_language = (lang == UI_LANG_TH) ? UI_LANG_TH : UI_LANG_EN;
        }
        nvs_close(h);
        
        if (va >= 0 && va <= 30) g_alert_volume = va;
        if (vn >= 0 && vn <= 30) g_nav_volume = vn;
        ESP_LOGI(TAG, "Settings loaded: alt=%d nav=%d en=%d lang=%d", g_alert_volume, g_nav_volume, g_nav_sound_enabled, g_ui_language);
    }
}

static void draw_icon_gear(int cx, int cy, uint16_t color, uint16_t bg)
{
    fill_round_rect(cx - 10, cy - 10, 20, 20, 10, color);
    fill_round_rect(cx - 8,  cy - 8,  16, 16, 8,  bg);
    fill_round_rect(cx - 4,  cy - 4,  8,  8,  4,  color);
    fill_rect(cx - 2, cy - 13, 4, 6, color);
    fill_rect(cx - 2, cy + 7,  4, 6, color);
    fill_rect(cx - 13, cy - 2, 6, 4, color);
    fill_rect(cx + 7,  cy - 2, 6, 4, color);
}

// Layout definitions
#define VOL1_CARD_Y  60
#define VOL2_CARD_Y  148
#define CARD_H       80

#define VOL_BTN_W    36
#define VOL_BTN_H    30

static int settings_minus_x(void)
{
    return CARD_X + CARD_INSET_X;
}

static int settings_plus_x(bool is_alert)
{
    const int card_right = CARD_X + CARD_W;
    const int toggle_w = is_alert ? 0 : 46 + 8;
    return card_right - CARD_RIGHT_PAD - toggle_w - VOL_BTN_W;
}

static void draw_volume_card(int y_offset, const char *title, int vol, bool is_alert)
{
    fill_round_rect(CARD_X, y_offset, CARD_W, CARD_H, 8, SB_COLOR_CARD);
    
    // Title
    if (g_ui_language == UI_LANG_TH) {
        draw_label_bitmap(CARD_X + 10, y_offset + 6, is_alert ? &kThAlertVolume : &kThNavVolume);
    } else {
        draw_string_gfx(CARD_X + 10, y_offset + 22, title, SB_COLOR_TXT_MAIN, SB_COLOR_CARD, &FreeSans12pt7b);
    }
    
    // Status text (Value or Disabled)
    char vol_str[16];
    if (!is_alert && !g_nav_sound_enabled) {
        snprintf(vol_str, sizeof(vol_str), "OFF");
    } else {
        snprintf(vol_str, sizeof(vol_str), "%d / 30", vol);
    }
    
    int16_t val_w = gfx_text_width(vol_str, &FreeSans12pt7b);
    uint16_t val_col = (!is_alert && !g_nav_sound_enabled) ? SB_COLOR_TXT_MUTED : THEME_OK;
    
    // Erase old value background context
    const int16_t card_right = CARD_X + CARD_W;
    fill_rect(card_right - val_w - 20, y_offset + 4, val_w + 16, 24, SB_COLOR_CARD);
    if (g_ui_language == UI_LANG_TH && !is_alert && !g_nav_sound_enabled) {
        draw_label_bitmap(card_right - kThValueOff.width - 10, y_offset + 6, &kThValueOff);
    } else {
        draw_string_gfx(card_right - val_w - 10, y_offset + 22, vol_str, val_col, SB_COLOR_CARD, &FreeSans12pt7b);
    }
    
    int btn_y = y_offset + 38;
    int minus_x = settings_minus_x();
    int plus_x = settings_plus_x(is_alert);
    
    // Plus / Minus
    uint16_t btn_bg = (!is_alert && !g_nav_sound_enabled) ? SB_COLOR_BG : SB_COLOR_MUTED;
    fill_round_rect(minus_x, btn_y, VOL_BTN_W, VOL_BTN_H, 6, btn_bg);
    draw_string_centered(minus_x + VOL_BTN_W/2, btn_y + 21, "-", 0xFFFF, btn_bg, &FreeSansBold18pt7b);
    
    btn_bg = (!is_alert && !g_nav_sound_enabled) ? SB_COLOR_BG : THEME_WARN;
    fill_round_rect(plus_x, btn_y, VOL_BTN_W, VOL_BTN_H, 6, btn_bg);
    draw_string_centered(plus_x + VOL_BTN_W/2, btn_y + 21, "+", 0xFFFF, btn_bg, &FreeSansBold18pt7b);
    
    // Toggle Button for NAV
    if (!is_alert) {
        fill_round_rect(plus_x + VOL_BTN_W + 8, btn_y, 46, VOL_BTN_H, 6, g_nav_sound_enabled ? THEME_OK : THEME_BAD);
        if (g_ui_language == UI_LANG_TH) {
            const ui_label_bitmap_t *toggle_label = g_nav_sound_enabled ? &kThOn : &kThOff;
            draw_label_bitmap(plus_x + VOL_BTN_W + 31 - (toggle_label->width / 2), btn_y + 6, toggle_label);
        } else {
            draw_string_centered(plus_x + VOL_BTN_W + 31, btn_y + 21, g_nav_sound_enabled ? "ON" : "OFF", 0xFFFF, g_nav_sound_enabled ? THEME_OK : THEME_BAD, &FreeSans9pt7b);
        }
    }
    
    // Progress bar
    int bar_x = minus_x + VOL_BTN_W + 10;
    int bar_w = plus_x - bar_x - 10;
    int bar_y = btn_y + 4;
    int bar_h = 22;
    
    fill_round_rect(bar_x, bar_y, bar_w, bar_h, 4, THEME_INACTIVE);
    if ((is_alert || g_nav_sound_enabled) && vol > 0) {
        int filled = (vol * bar_w) / 30;
        if (filled < 8) {
            fill_rect(bar_x, bar_y, filled, bar_h, THEME_OK);
        } else {
            fill_round_rect(bar_x, bar_y, filled, bar_h, 4, THEME_OK);
        }
    }
}

// ── Bottom buttons ─────────────────────────────────────────────────
#define TEST_ALARM_X  22
#define TEST_ALARM_Y  258
#define TEST_ALARM_W  200
#define TEST_NAV_X    (LCD_W - 22 - 200)
#define TEST_NAV_Y    258
#define TEST_NAV_W    200
#define BTN_H_ROW     36

static void draw_bottom_buttons(void)
{
    fill_round_rect(TEST_ALARM_X, TEST_ALARM_Y, TEST_ALARM_W, BTN_H_ROW, 10, THEME_BORDER);
    if (g_ui_language == UI_LANG_TH) {
        draw_label_bitmap(TEST_ALARM_X + ((TEST_ALARM_W - kThTestAlarm.width) / 2), TEST_ALARM_Y + 8, &kThTestAlarm);
    } else {
        draw_string_centered(TEST_ALARM_X + TEST_ALARM_W / 2, TEST_ALARM_Y + 24, "TEST ALARM", 0xFFFF, THEME_BORDER, &FreeSans9pt7b);
    }

    fill_round_rect(TEST_NAV_X, TEST_NAV_Y, TEST_NAV_W, BTN_H_ROW, 10, SB_COLOR_PRIMARY);
    if (g_ui_language == UI_LANG_TH) {
        draw_label_bitmap(TEST_NAV_X + ((TEST_NAV_W - kThTestNav.width) / 2), TEST_NAV_Y + 8, &kThTestNav);
    } else {
        draw_string_centered(TEST_NAV_X + TEST_NAV_W / 2, TEST_NAV_Y + 24, "TEST NAV SOUND", 0xFFFF, SB_COLOR_PRIMARY, &FreeSans9pt7b);
    }
}

void ui_settings_render(void)
{
    if (!force_redraw) return;

    fill_screen(SB_COLOR_BG);
    draw_top_bar_with_back(g_ui_language == UI_LANG_TH ? NULL : "Settings");
    if (g_ui_language == UI_LANG_TH) {
        draw_label_bitmap((LCD_W - kThTopSettings.width) / 2, 8, &kThTopSettings);
    }
    draw_icon_gear(455, 25, SB_COLOR_TXT_MUTED, SB_COLOR_BG);
    fill_rect(22, 58, 436, 2, SB_COLOR_BORDER);

    draw_volume_card(VOL1_CARD_Y, "Alert Volume (Alarms)", g_alert_volume, true);
    draw_volume_card(VOL2_CARD_Y, "Navigation Sounds", g_nav_volume, false);
    
    draw_bottom_buttons();
    force_redraw = false;
}

void ui_settings_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    (void)now_ms;

    if (ty_n < 50) { dfplayer_play_track(10); pending_page = PAGE_MENU; return; }

    bool vol_changed = false;

    // Alert Volume
    int btn_y = VOL1_CARD_Y + 38;
    int minus_x = settings_minus_x();
    int plus_x = settings_plus_x(true);
    if (ty_n >= btn_y && ty_n <= btn_y + VOL_BTN_H) {
        if (tx_n >= minus_x && tx_n <= minus_x + VOL_BTN_W) {
            if (g_alert_volume > 0) { g_alert_volume--; vol_changed = true; }
        } else if (tx_n >= plus_x && tx_n <= plus_x + VOL_BTN_W) {
            if (g_alert_volume < 30) { g_alert_volume++; vol_changed = true; }
        }
    }
    
    // Nav Volume
    btn_y = VOL2_CARD_Y + 38;
    plus_x = settings_plus_x(false);
    if (ty_n >= btn_y && ty_n <= btn_y + VOL_BTN_H) {
        if (tx_n >= minus_x && tx_n <= minus_x + VOL_BTN_W) {
            if (g_nav_sound_enabled && g_nav_volume > 0) { g_nav_volume--; vol_changed = true; }
        } else if (tx_n >= plus_x && tx_n <= plus_x + VOL_BTN_W) {
            if (g_nav_sound_enabled && g_nav_volume < 30) { g_nav_volume++; vol_changed = true; }
        } else if (tx_n >= plus_x + VOL_BTN_W + 8 && tx_n <= plus_x + VOL_BTN_W + 54) {
            g_nav_sound_enabled = !g_nav_sound_enabled;
            vol_changed = true;
            if (g_nav_sound_enabled) {
                dfplayer_play_track(35); // Track 35: Navigation Sound ON
            } else {
                dfplayer_play_track(36); // Track 36: Navigation Sound OFF (plays as alert, won't be suppressed)
            }
        }
    }

    if (tx_n >= TEST_ALARM_X && tx_n <= (TEST_ALARM_X + TEST_ALARM_W) &&
        ty_n >= TEST_ALARM_Y && ty_n <= (TEST_ALARM_Y + BTN_H_ROW)) {
        dfplayer_stop();
        dfplayer_play_track(1); // Test Alarm sound
        return;
    }
    else if (tx_n >= TEST_NAV_X && tx_n <= (TEST_NAV_X + TEST_NAV_W) &&
             ty_n >= TEST_NAV_Y && ty_n <= (TEST_NAV_Y + BTN_H_ROW)) {
        dfplayer_stop();
        dfplayer_play_track(10); // Test Navigation sound (back button sound)
        return;
    }

    if (vol_changed) {
        draw_volume_card(VOL1_CARD_Y, "Alert Volume (Alarms)", g_alert_volume, true);
        draw_volume_card(VOL2_CARD_Y, "Navigation Sounds", g_nav_volume, false);
        settings_save_nvs(); // Auto-save whenever user adjusts volume
    }
}
