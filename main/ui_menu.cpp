#include "ui_core.h"
#include "dfplayer.h"
#include "ui_menu_thai_labels.h"
#include "ui_utf8_text.h"

typedef struct {
    const char *title_en;
    const char *subtitle_en;
    const ui_label_bitmap_t *title_th;
    const ui_label_bitmap_t *subtitle_th;
    const char *title_th_utf8;
    const char *subtitle_th_utf8;
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
        if (text->title_th_utf8 && text->title_th_utf8[0]) {
            ui_utf8_draw_text(x, title_y + 4, text->title_th_utf8, SB_COLOR_TXT_MAIN);
        } else {
            draw_bitmap_label(x, title_y, text->title_th);
        }

        if (text->subtitle_th_utf8 && text->subtitle_th_utf8[0]) {
            ui_utf8_draw_text(x, subtitle_y + 2, text->subtitle_th_utf8, SB_COLOR_TXT_MUTED);
        } else {
            draw_bitmap_label(x, subtitle_y, text->subtitle_th);
        }
        return;
    }

    draw_label_boldish(x, title_y + 14, text->title_en, SB_COLOR_TXT_MAIN, SB_COLOR_CARD, &FreeSans12pt7b);
    draw_string_gfx(x, subtitle_y + 8, text->subtitle_en, SB_COLOR_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
}

static void draw_language_toggle(void)
{
    const int chip_y = 16;
    const int chip_h = 36;
    const int chip_w = 62;
    const int chip_gap = 6;
    const int en_x = LCD_W - 146;
    const int th_x = en_x + chip_w + chip_gap;

    fill_round_rect_frame(en_x, chip_y, chip_w, chip_h, 8,
                          g_ui_language == UI_LANG_EN ? ST_RGB565(34, 197, 94) : THEME_PANEL,
                          g_ui_language == UI_LANG_EN ? ST_RGB565(34, 197, 94) : THEME_INACTIVE);
    draw_string_centered(en_x + (chip_w / 2), chip_y + 24, "EN", 0xFFFF,
                         g_ui_language == UI_LANG_EN ? ST_RGB565(34, 197, 94) : THEME_PANEL,
                         &FreeSans9pt7b);

    fill_round_rect_frame(th_x, chip_y, chip_w, chip_h, 8,
                          g_ui_language == UI_LANG_TH ? ST_RGB565(34, 197, 94) : THEME_PANEL,
                          g_ui_language == UI_LANG_TH ? ST_RGB565(34, 197, 94) : THEME_INACTIVE);
    int16_t th_tw = ui_utf8_text_width("ไทย");
    ui_utf8_draw_text(th_x + (chip_w / 2) - (th_tw / 2), chip_y + 3, "ไทย", 0xFFFF);
}

static void draw_icon_clock(int cx, int cy, uint16_t color, uint16_t bg_color)
{
    // Base: rounded square clock face
    fill_round_rect(cx - 27, cy - 27, 54, 54, 18, color);
    // Inner cutout
    fill_round_rect(cx - 21, cy - 21, 42, 42, 14, bg_color);
    
    // Clock hands
    fill_round_rect(cx - 3, cy - 12, 6, 15, 3, color); // Hour hand (12)
    fill_round_rect(cx - 3, cy - 3, 15, 6, 3, color); // Minute hand (3)
    
    // Center dot
    fill_round_rect(cx - 4, cy - 4, 8, 8, 4, color);
}

static void draw_icon_capsule(int cx, int cy, uint16_t color, uint16_t bg_color)
{
    // Pill outline
    fill_round_rect(cx - 30, cy - 15, 60, 30, 15, color);
    fill_round_rect(cx - 24, cy - 9, 48, 18, 9, bg_color);
    
    // Middle split
    fill_rect(cx - 3, cy - 15, 6, 30, color);
    
    // Left side mark (a tiny plus)
    fill_rect(cx - 16, cy - 4, 8, 3, color);
    fill_rect(cx - 13, cy - 7, 3, 9, color);
}

static void draw_icon_gear(int cx, int cy, uint16_t color, uint16_t bg_color)
{
    // Solid round body first — teeth attach onto it as sharp rectangles
    fill_round_rect(cx - 18, cy - 18, 36, 36, 18, color);

    // 4 cardinal teeth — trapezoidal (wider base, narrower tip) using two stacked rects
    // Top
    fill_rect(cx - 7, cy - 24, 14, 8, color);
    fill_rect(cx - 5, cy - 28, 10, 5, color);
    // Bottom
    fill_rect(cx - 7, cy + 16, 14, 8, color);
    fill_rect(cx - 5, cy + 23, 10, 5, color);
    // Left
    fill_rect(cx - 24, cy - 7, 8, 14, color);
    fill_rect(cx - 28, cy - 5, 5, 10, color);
    // Right
    fill_rect(cx + 16, cy - 7, 8, 14, color);
    fill_rect(cx + 23, cy - 5, 5, 10, color);

    // 4 diagonal teeth — stepped rectangles to approximate 45° tips
    // Top-right
    fill_rect(cx + 11, cy - 19, 7, 7, color);
    fill_rect(cx + 15, cy - 22, 5, 5, color);
    // Top-left
    fill_rect(cx - 18, cy - 19, 7, 7, color);
    fill_rect(cx - 20, cy - 22, 5, 5, color);
    // Bottom-right
    fill_rect(cx + 11, cy + 12, 7, 7, color);
    fill_rect(cx + 15, cy + 17, 5, 5, color);
    // Bottom-left
    fill_rect(cx - 18, cy + 12, 7, 7, color);
    fill_rect(cx - 20, cy + 17, 5, 5, color);

    // Hollow ring cutout — thick rim makes it read as a mechanical cog
    fill_round_rect(cx - 10, cy - 10, 20, 20, 10, bg_color);

    // Central spindle hub
    fill_round_rect(cx - 6, cy - 6, 12, 12, 6, color);
    // Axle hole
    fill_round_rect(cx - 3, cy - 3, 6, 6, 3, bg_color);
}

static bool menu_back_hit(uint16_t x, uint16_t y)
{
    return (x >= 0 && x <= 130 && y >= 0 && y <= 52);
}

static void draw_icon_wifi(int cx, int cy, uint16_t color, uint16_t bg_color)
{
    // Arc 3 (Outer)
    fill_round_rect(cx - 33, cy - 15, 66, 66, 33, color);
    fill_round_rect(cx - 24, cy - 6, 48, 48, 24, bg_color);
    
    // Arc 2 (Middle)
    fill_round_rect(cx - 15, cy + 3, 30, 30, 15, color);
    
    // Wipe the bottom half
    fill_rect(cx - 33, cy + 15, 66, 36, bg_color);
    
    // Dot (Inner)
    fill_round_rect(cx - 6, cy + 12, 12, 12, 6, color);
}

void ui_menu_render(void)
{
    if (!force_redraw) return;

    static const menu_card_text_t kScheduleText = {
        "Schedule", "Set med times",
        &kMenuThTitleSchedule, &kMenuThSubSchedule, NULL, NULL
    };
    static const menu_card_text_t kMedicineText = {
        "Medicine", "Manage slots",
        &kMenuThTitleMedicine, &kMenuThSubMedicine, NULL, NULL
    };
    static const menu_card_text_t kSettingsText = {
        "System", "Sound & status",
        &kMenuThTitleSettings, NULL, "ตั้งค่า", "เสียงและสถานะ"
    };
    static const menu_card_text_t kWifiText = {
        "WiFi", "Network setup",
        &kMenuThTitleWifi, &kMenuThSubWifi, NULL, NULL
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
    draw_top_bar_with_back(NULL);
    if (g_ui_language == UI_LANG_TH) {
        draw_utf8_centered_line_scaled(LCD_W / 2, 16, "เมนูหลัก", THEME_TXT_MAIN, THEME_PANEL, 30);
    }
    if (g_ui_language != UI_LANG_TH) {
        draw_label_boldish(140, 39, "Setup Menu", THEME_TXT_MAIN, THEME_PANEL, &FreeSans12pt7b);
    }
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
    if (ty_n < 56) {
        if (ty_n >= 14 && ty_n <= 54) {
            if (tx_n >= (LCD_W - 146) && tx_n <= (LCD_W - 84)) {
                if (g_ui_language != UI_LANG_EN) {
                    g_ui_language = UI_LANG_EN;
                    dfplayer_set_language(1);
                    dfplayer_play_track(81);
                    settings_save_nvs();
                    force_redraw = true;
                }
                return;
            }
            if (tx_n >= (LCD_W - 78) && tx_n <= (LCD_W - 16)) {
                if (g_ui_language != UI_LANG_TH) {
                    g_ui_language = UI_LANG_TH;
                    dfplayer_set_language(0);
                    dfplayer_play_track(80);
                    settings_save_nvs();
                    force_redraw = true;
                }
                return;
            }
        }

        if (menu_back_hit(tx_n, ty_n)) {
            dfplayer_play_track(g_snd_button);
            pending_page = PAGE_STANDBY;
        }
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
