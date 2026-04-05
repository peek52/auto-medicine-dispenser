#include "ui_core.h"
#include "dfplayer.h"
#include "ui_menu_thai_labels.h"

typedef struct {
    const char *title_en;
    const char *subtitle_en;
    const ui_label_bitmap_t *title_th;
    const ui_label_bitmap_t *subtitle_th;
} menu_card_text_t;

static void draw_label_boldish(int16_t x, int16_t y, const char *text, uint16_t fg, uint16_t bg, const GFXfont *font)
{
    draw_string_gfx(x, y, text, fg, bg, font);
    draw_string_gfx(x + 1, y, text, fg, bg, font);
}

static void draw_bitmap_label(int16_t x, int16_t y, const ui_label_bitmap_t *label)
{
    if (!label || !label->pixels) return;

    static const uint16_t kTransparentKey = SB_COLOR_CARD;
    for (int16_t row = 0; row < label->height; ++row) {
        const uint16_t *src = label->pixels + (row * label->width);
        int16_t run_start = -1;

        for (int16_t col = 0; col <= label->width; ++col) {
            bool opaque = false;
            if (col < label->width) {
                opaque = (src[col] != kTransparentKey);
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

static void draw_menu_label(int16_t x, int16_t title_y, int16_t subtitle_y, const menu_card_text_t *text)
{
    if (g_ui_language == UI_LANG_TH) {
        draw_bitmap_label(x, title_y, text->title_th);
        draw_bitmap_label(x, subtitle_y, text->subtitle_th);
        return;
    }

    draw_label_boldish(x, title_y + 14, text->title_en, SB_COLOR_TXT_MAIN, SB_COLOR_CARD, &FreeSans12pt7b);
    draw_string_gfx(x, subtitle_y + 8, text->subtitle_en, SB_COLOR_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
}

static void draw_language_toggle(void)
{
    const int chip_y = 14;
    const int chip_h = 24;
    const int chip_w = 38;
    const int chip_gap = 8;
    const int en_x = LCD_W - 104;
    const int th_x = en_x + chip_w + chip_gap;

    fill_round_rect_frame(en_x, chip_y, chip_w, chip_h, 12,
                          g_ui_language == UI_LANG_EN ? SB_COLOR_PRIMARY : THEME_PANEL,
                          SB_COLOR_BORDER);
    draw_string_centered(en_x + (chip_w / 2), chip_y + 17, "EN", 0xFFFF,
                         g_ui_language == UI_LANG_EN ? SB_COLOR_PRIMARY : THEME_PANEL,
                         &FreeSans9pt7b);

    fill_round_rect_frame(th_x, chip_y, chip_w, chip_h, 12,
                          g_ui_language == UI_LANG_TH ? SB_COLOR_PRIMARY : THEME_PANEL,
                          SB_COLOR_BORDER);
    draw_string_centered(th_x + (chip_w / 2), chip_y + 17, "TH", 0xFFFF,
                         g_ui_language == UI_LANG_TH ? SB_COLOR_PRIMARY : THEME_PANEL,
                         &FreeSans9pt7b);
}

static void draw_icon_clock(int cx, int cy, uint16_t color, uint16_t bg_color)
{
    fill_round_rect_frame(cx - 18, cy - 18, 36, 36, 18, bg_color, color);
    fill_round_rect_frame(cx - 17, cy - 17, 34, 34, 17, bg_color, color);
    fill_round_rect_frame(cx - 16, cy - 16, 32, 32, 16, bg_color, color);
    fill_round_rect_frame(cx - 15, cy - 15, 30, 30, 15, bg_color, color);
    fill_rect(cx - 2, cy - 10, 4, 12, color);
    fill_rect(cx - 2, cy - 2, 12, 4, color);
}

static void draw_icon_capsule(int cx, int cy, uint16_t color, uint16_t bg_color)
{
    fill_round_rect_frame(cx - 20, cy - 10, 40, 20, 10, bg_color, color);
    fill_round_rect_frame(cx - 19, cy - 9, 38, 18, 9, bg_color, color);
    fill_round_rect_frame(cx - 18, cy - 8, 36, 16, 8, bg_color, color);
    fill_round_rect_frame(cx - 17, cy - 7, 34, 14, 7, bg_color, color);
    fill_rect(cx - 2, cy - 10, 4, 20, color);
    fill_round_rect(cx + 6, cy - 3, 6, 6, 3, color);
}

static void draw_icon_gear(int cx, int cy, uint16_t color, uint16_t bg_color)
{
    fill_rect(cx - 4, cy - 18, 8, 36, color);
    fill_rect(cx - 18, cy - 4, 36, 8, color);
    fill_round_rect(cx - 12, cy - 12, 24, 24, 12, color);
    fill_round_rect(cx - 5, cy - 5, 10, 10, 5, bg_color);
}

static void draw_icon_wifi(int cx, int cy, uint16_t color, uint16_t bg_color)
{
    (void)bg_color;
    fill_round_rect(cx - 4, cy + 8, 8, 8, 4, color);
    fill_round_rect(cx - 12, cy, 24, 6, 3, color);
    fill_round_rect(cx - 20, cy - 10, 40, 6, 3, color);
}

void ui_menu_render(void)
{
    if (!force_redraw) return;

    static const menu_card_text_t kScheduleText = {
        "Schedule", "Set med times",
        &kMenuThTitleSchedule, &kMenuThSubSchedule
    };
    static const menu_card_text_t kMedicineText = {
        "Medicine", "Manage slots",
        &kMenuThTitleMedicine, &kMenuThSubMedicine
    };
    static const menu_card_text_t kSettingsText = {
        "Settings", "Sound & alerts",
        &kMenuThTitleSettings, &kMenuThSubSettings
    };
    static const menu_card_text_t kWifiText = {
        "WiFi", "Network setup",
        &kMenuThTitleWifi, &kMenuThSubWifi
    };

    const int w = 212;
    const int h = 110;
    const int gap_x = 16;
    const int gap_y = 16;
    const int x1 = 20;
    const int x2 = x1 + w + gap_x;
    const int y1 = 68;
    const int y2 = y1 + h + gap_y;

    fill_screen(THEME_BG);
    draw_top_bar_with_back("Setup Menu");
    draw_language_toggle();

    fill_round_rect(x1, y1, w, h, 14, SB_COLOR_CARD);
    fill_round_rect(x1, y1, w, 6, 3, SB_COLOR_PRIMARY);
    draw_icon_clock(x1 + 40, y1 + 52, SB_COLOR_PRIMARY, SB_COLOR_CARD);
    draw_menu_label(x1 + 70, y1 + 26, y1 + 58, &kScheduleText);

    fill_round_rect(x2, y1, w, h, 14, SB_COLOR_CARD);
    fill_round_rect(x2, y1, w, 6, 3, SB_COLOR_ACCENT);
    draw_icon_capsule(x2 + 40, y1 + 52, SB_COLOR_ACCENT, SB_COLOR_CARD);
    draw_menu_label(x2 + 70, y1 + 26, y1 + 58, &kMedicineText);

    fill_round_rect(x1, y2, w, h, 14, SB_COLOR_CARD);
    fill_round_rect(x1, y2, w, 6, 3, THEME_WARN);
    draw_icon_gear(x1 + 40, y2 + 52, THEME_WARN, SB_COLOR_CARD);
    draw_menu_label(x1 + 70, y2 + 26, y2 + 58, &kSettingsText);

    fill_round_rect(x2, y2, w, h, 14, SB_COLOR_CARD);
    fill_round_rect(x2, y2, w, 6, 3, COLOR_BTN_TEAL);
    draw_icon_wifi(x2 + 40, y2 + 52, COLOR_BTN_TEAL, SB_COLOR_CARD);
    draw_menu_label(x2 + 70, y2 + 26, y2 + 58, &kWifiText);

    force_redraw = false;
}

void ui_menu_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    if (ty_n >= 14 && ty_n <= 38) {
        if (tx_n >= (LCD_W - 104) && tx_n <= (LCD_W - 66)) {
            if (g_ui_language != UI_LANG_EN) {
                g_ui_language = UI_LANG_EN;
                settings_save_nvs();
                force_redraw = true;
            }
            return;
        }
        if (tx_n >= (LCD_W - 58) && tx_n <= (LCD_W - 20)) {
            if (g_ui_language != UI_LANG_TH) {
                g_ui_language = UI_LANG_TH;
                settings_save_nvs();
                force_redraw = true;
            }
            return;
        }
    }

    if (ty_n < 50) {
        dfplayer_play_track(10);
        pending_page = PAGE_STANDBY;
        return;
    }

    if (ty_n >= 68 && ty_n <= 178) {
        if (tx_n >= 20 && tx_n <= 232) {
            dfplayer_play_track(8);
            pending_page = PAGE_SETUP_SCHEDULE;
        } else if (tx_n >= 248 && tx_n <= 460) {
            dfplayer_play_track(7);
            pending_page = PAGE_SETUP_MEDS;
        }
    } else if (ty_n >= 194 && ty_n <= 304) {
        if (tx_n >= 20 && tx_n <= 232) {
            dfplayer_play_track(6);
            pending_page = PAGE_SETTINGS;
        } else if (tx_n >= 248 && tx_n <= 460) {
            dfplayer_play_track(5);
            pending_page = PAGE_WIFI_SCAN;
        }
    }
}
