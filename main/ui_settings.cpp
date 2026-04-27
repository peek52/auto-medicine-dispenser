#include "ui_core.h"
#include "dfplayer.h"
#include "nvs.h"
#include "esp_log.h"
#include "offline_sync.h"
#include "wifi_sta.h"
#include "telegram_bot.h"
#include "ui_settings_thai_labels.h"
#include "ui_utf8_text.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_settings";

int g_alert_volume       = 25;
int g_nav_volume         = 15;
bool g_nav_sound_enabled = true;
enum ui_language_t g_ui_language = UI_LANG_TH;

/* ── Sound track numbers (configurable via web) ── */
int g_snd_alarm     = 1;   // Main alarm
int g_snd_disp_th   = 83;  // TH: จ่ายยาสำเร็จ
int g_snd_return_th = 84;  // TH: จ่ายยาคืนเรียบร้อย
int g_snd_nomeds_th = 85;  // TH: ไม่พบยา
int g_snd_disp_en   = 86;  // EN: Dispensed successfully
int g_snd_return_en = 87;  // EN: Returned successfully
int g_snd_nomeds_en = 88;  // EN: No medication detected
int g_snd_button    = 10;  // Button click sound
int g_snd_volup_th  = 95;  // TH: เพิ่ม
int g_snd_volup_en  = 97;  // EN: increase
int g_snd_voldn_th  = 96;  // TH: ลด
int g_snd_voldn_en  = 98;  // EN: decrease

#define CARD_X          12
#define CARD_W          (LCD_W - 24)
#define CONTENT_TOP     52
#define VOL_CARD_H      72
#define STATUS_CARD_H   96
#define CARD_GAP        10

#define ALERT_Y         CONTENT_TOP
#define NAV_Y           (ALERT_Y + VOL_CARD_H + CARD_GAP)
#define STATUS_Y        (NAV_Y + VOL_CARD_H + CARD_GAP)

#define BTN_W           34
#define BTN_H           28
#define BTN_ROW_Y_OFF   34
#define BAR_H           20
#define TOGGLE_W        44

#define THAI_TEXT_NUDGE     -4
#define THAI_VALUE_NUDGE    -3
#define STATUS_TITLE_Y      18
#define STATUS_ROW1_Y       36
#define STATUS_ROW2_Y       56
#define STATUS_ROW3_Y       76

static bool s_settings_layout_ready = false;
static bool s_settings_alert_dirty = true;
static bool s_settings_nav_dirty = true;
static bool s_settings_status_dirty = true;

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

static int16_t draw_utf8_text_line(int16_t x, int16_t y, const char *text, uint16_t color)
{
    return ui_utf8_draw_text(x, y, text, color);
}

void settings_save_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i16(h, "vol_alt", (int16_t)g_alert_volume);
        nvs_set_i16(h, "vol_nav", (int16_t)g_nav_volume);
        nvs_set_u8(h, "en_nav", (uint8_t)g_nav_sound_enabled);
        nvs_set_u8(h, "lang_ui", (uint8_t)g_ui_language);
        nvs_set_i16(h, "snd_alarm",  (int16_t)g_snd_alarm);
        nvs_set_i16(h, "snd_dth",    (int16_t)g_snd_disp_th);
        nvs_set_i16(h, "snd_rth",    (int16_t)g_snd_return_th);
        nvs_set_i16(h, "snd_nth",    (int16_t)g_snd_nomeds_th);
        nvs_set_i16(h, "snd_den",    (int16_t)g_snd_disp_en);
        nvs_set_i16(h, "snd_ren",    (int16_t)g_snd_return_en);
        nvs_set_i16(h, "snd_nen",    (int16_t)g_snd_nomeds_en);
        nvs_set_i16(h, "snd_btn",    (int16_t)g_snd_button);
        nvs_set_i16(h, "snd_vupth",  (int16_t)g_snd_volup_th);
        nvs_set_i16(h, "snd_vupen",  (int16_t)g_snd_volup_en);
        nvs_set_i16(h, "snd_vdnth",  (int16_t)g_snd_voldn_th);
        nvs_set_i16(h, "snd_vden",   (int16_t)g_snd_voldn_en);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Settings saved: alt=%d nav=%d en=%d lang=%d",
                 g_alert_volume, g_nav_volume, g_nav_sound_enabled, g_ui_language);
    }

    telegram_set_language((g_ui_language == UI_LANG_TH) ? TELEGRAM_LANG_TH : TELEGRAM_LANG_EN);
}

void settings_load_nvs(void)
{
    g_ui_language = UI_LANG_TH;
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READONLY, &h) == ESP_OK) {
        int16_t va = g_alert_volume;
        int16_t vn = g_nav_volume;
        uint8_t en = g_nav_sound_enabled;
        int16_t sa = g_snd_alarm, sdth = g_snd_disp_th, srth = g_snd_return_th, snth = g_snd_nomeds_th;
        int16_t sden = g_snd_disp_en, sren = g_snd_return_en, snen = g_snd_nomeds_en;

        nvs_get_i16(h, "vol_alt", &va);
        nvs_get_i16(h, "vol_nav", &vn);
        if (nvs_get_u8(h, "en_nav", &en) == ESP_OK) {
            g_nav_sound_enabled = (en > 0);
        }
        nvs_get_i16(h, "snd_alarm", &sa);
        nvs_get_i16(h, "snd_dth",   &sdth);
        nvs_get_i16(h, "snd_rth",   &srth);
        nvs_get_i16(h, "snd_nth",   &snth);
        nvs_get_i16(h, "snd_den",   &sden);
        nvs_get_i16(h, "snd_ren",   &sren);
        nvs_get_i16(h, "snd_nen",   &snen);
        nvs_close(h);

        if (va >= 0 && va <= 30) g_alert_volume = va;
        if (vn >= 0 && vn <= 30) g_nav_volume = vn;
        if (sa > 0 && sa < 200) g_snd_alarm     = sa;
        if (sdth > 0 && sdth < 200) g_snd_disp_th   = sdth;
        if (srth > 0 && srth < 200) g_snd_return_th = srth;
        if (snth > 0 && snth < 200) g_snd_nomeds_th = snth;
        if (sden > 0 && sden < 200) g_snd_disp_en   = sden;
        if (sren > 0 && sren < 200) g_snd_return_en = sren;
        if (snen > 0 && snen < 200) g_snd_nomeds_en = snen;
        int16_t sbtn = 10;
        nvs_get_i16(h, "snd_btn", &sbtn);
        if (sbtn > 0 && sbtn < 200) g_snd_button = sbtn;

        int16_t vupth = g_snd_volup_th, vupen = g_snd_volup_en;
        int16_t vdnth = g_snd_voldn_th, vden  = g_snd_voldn_en;
        nvs_get_i16(h, "snd_vupth", &vupth);
        nvs_get_i16(h, "snd_vupen", &vupen);
        nvs_get_i16(h, "snd_vdnth", &vdnth);
        nvs_get_i16(h, "snd_vden",  &vden);
        if (vupth > 0 && vupth < 200) g_snd_volup_th = vupth;
        if (vupen > 0 && vupen < 200) g_snd_volup_en = vupen;
        if (vdnth > 0 && vdnth < 200) g_snd_voldn_th = vdnth;
        if (vden  > 0 && vden  < 200) g_snd_voldn_en = vden;
        ESP_LOGI(TAG, "Settings loaded: alt=%d nav=%d en=%d lang=%d",
                 g_alert_volume, g_nav_volume, g_nav_sound_enabled, g_ui_language);
    }

    telegram_set_language((g_ui_language == UI_LANG_TH) ? TELEGRAM_LANG_TH : TELEGRAM_LANG_EN);
    dfplayer_set_language(g_ui_language == UI_LANG_EN ? 1 : 0);
}

static int settings_minus_x(void)
{
    return CARD_X + 12;
}

static int settings_plus_x(bool is_alert)
{
    int reserve = is_alert ? 0 : (TOGGLE_W + 8);
    return CARD_X + CARD_W - 12 - reserve - BTN_W;
}

static int settings_toggle_x(void)
{
    return settings_plus_x(false) + BTN_W + 8;
}

static void draw_card_shell(int y, int h, uint16_t accent)
{
    fill_round_rect(CARD_X, y, CARD_W, h, 10, SB_COLOR_CARD);
    fill_round_rect(CARD_X, y, CARD_W, 4, 3, accent);
}

static void draw_volume_title(int y, bool is_alert)
{
    if (g_ui_language == UI_LANG_TH) {
        draw_utf8_text_line(CARD_X + 12, y + 18 + THAI_TEXT_NUDGE,
                            is_alert ? "เสียงเตือน" : "เสียงปุ่ม",
                            SB_COLOR_TXT_MAIN);
    } else {
        draw_string_gfx(CARD_X + 12, y + 20,
                        is_alert ? "Alarm sound" : "Button sound",
                        SB_COLOR_TXT_MAIN, SB_COLOR_CARD, &FreeSans9pt7b);
    }
}

static void draw_volume_value(int y, int vol, bool is_alert)
{
    char value_str[16];
    const bool is_off = (!is_alert && !g_nav_sound_enabled);

    if (is_off) {
        safe_copy(value_str, sizeof(value_str), "OFF");
    } else {
        snprintf(value_str, sizeof(value_str), "%d/30", vol);
    }

    if (g_ui_language == UI_LANG_TH && is_off) {
        draw_label_bitmap(CARD_X + CARD_W - kThValueOff.width - 12, y + 8, &kThValueOff);
        return;
    }

    int16_t w = (g_ui_language == UI_LANG_TH)
                    ? ui_utf8_text_width(value_str)
                    : gfx_text_width(value_str, &FreeSans9pt7b);
    int16_t x = CARD_X + CARD_W - w - 12;

    if (g_ui_language == UI_LANG_TH) {
        draw_utf8_text_line(x, y + 18 + THAI_VALUE_NUDGE, value_str,
                            is_off ? SB_COLOR_TXT_MUTED : THEME_OK);
    } else {
        draw_string_gfx(x, y + 20, value_str,
                        is_off ? SB_COLOR_TXT_MUTED : THEME_OK,
                        SB_COLOR_CARD, &FreeSans9pt7b);
    }
}

static void draw_volume_controls(int y, int vol, bool is_alert)
{
    int btn_y = y + BTN_ROW_Y_OFF;
    int minus_x = settings_minus_x();
    int plus_x = settings_plus_x(is_alert);
    int bar_x = minus_x + BTN_W + 10;
    int bar_y = btn_y + 4;
    int bar_w = plus_x - bar_x - 10;
    uint16_t btn_bg = (!is_alert && !g_nav_sound_enabled) ? SB_COLOR_BG : SB_COLOR_MUTED;
    uint16_t plus_bg = (!is_alert && !g_nav_sound_enabled) ? SB_COLOR_BG : THEME_WARN;

    fill_round_rect(minus_x, btn_y, BTN_W, BTN_H, 6, btn_bg);
    fill_round_rect(plus_x, btn_y, BTN_W, BTN_H, 6, plus_bg);
    draw_string_centered(minus_x + (BTN_W / 2), btn_y + 19, "-", 0xFFFF, btn_bg, &FreeSans12pt7b);
    draw_string_centered(plus_x + (BTN_W / 2), btn_y + 19, "+", 0xFFFF, plus_bg, &FreeSans12pt7b);

    fill_round_rect(bar_x, bar_y, bar_w, BAR_H, 4, THEME_INACTIVE);
    if ((is_alert || g_nav_sound_enabled) && vol > 0) {
        int filled = (vol * bar_w) / 30;
        if (filled < 8) {
            fill_rect(bar_x, bar_y, filled, BAR_H, THEME_OK);
        } else {
            fill_round_rect(bar_x, bar_y, filled, BAR_H, 4, THEME_OK);
        }
    }

    if (!is_alert) {
        int toggle_x = settings_toggle_x();
        uint16_t toggle_bg = g_nav_sound_enabled ? THEME_OK : THEME_BAD;
        fill_round_rect(toggle_x, btn_y, TOGGLE_W, BTN_H, 6, toggle_bg);
        if (g_ui_language == UI_LANG_TH) {
            const ui_label_bitmap_t *toggle_label = g_nav_sound_enabled ? &kThOn : &kThOff;
            draw_label_bitmap(toggle_x + (TOGGLE_W - toggle_label->width) / 2, btn_y + 8, toggle_label);
        } else {
            draw_string_centered(toggle_x + (TOGGLE_W / 2), btn_y + 19,
                                 g_nav_sound_enabled ? "ON" : "OFF",
                                 0xFFFF, toggle_bg, &FreeSans9pt7b);
        }
    }
}

static void draw_volume_card(int y, int vol, bool is_alert)
{
    draw_card_shell(y, VOL_CARD_H, is_alert ? THEME_WARN : SB_COLOR_PRIMARY);
    draw_volume_title(y, is_alert);
    draw_volume_value(y, vol, is_alert);
    draw_volume_controls(y, vol, is_alert);
}

static void draw_status_label(int16_t x, int16_t y, const char *text)
{
    if (g_ui_language == UI_LANG_TH) {
        draw_utf8_text_line(x, y, text, SB_COLOR_TXT_MUTED);
    } else {
        draw_string_gfx(x, y + 10, text, SB_COLOR_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
    }
}

static void draw_status_value(int16_t x, int16_t y, const char *text, uint16_t color)
{
    if (g_ui_language == UI_LANG_TH) {
        draw_utf8_text_line(x, y, text, color);
    } else {
        draw_string_gfx(x, y + 10, text, color, SB_COLOR_CARD, &FreeSans9pt7b);
    }
}

static void draw_system_status_card(int y)
{
    char network_value[24] = "";
    char address_value[32] = "";
    char sync_value[24] = "";

    bool wifi_online = wifi_sta_connected() && strcmp(s_ip, "0.0.0.0") != 0;
    bool ap_mode = (!wifi_online && strcmp(s_ip, "192.168.4.1") == 0);
    size_t pending_events = offline_sync_pending_event_count();

    uint16_t network_color = SB_COLOR_OK;
    uint16_t sync_color = SB_COLOR_OK;

    if (g_ui_language == UI_LANG_TH) {
        if (wifi_online) {
            safe_copy(network_value, sizeof(network_value), "ออนไลน์");
            safe_copy(address_value, sizeof(address_value), s_ip);
        } else if (ap_mode) {
            safe_copy(network_value, sizeof(network_value), "ตั้งค่า");
            safe_copy(address_value, sizeof(address_value), "192.168.4.1");
            network_color = THEME_WARN;
        } else {
            safe_copy(network_value, sizeof(network_value), "ออฟไลน์");
            safe_copy(address_value, sizeof(address_value), "-");
            network_color = THEME_WARN;
        }

        if (wifi_online) {
            safe_copy(sync_value, sizeof(sync_value), "พร้อม");
        } else if (pending_events > 0) {
            safe_copy(sync_value, sizeof(sync_value), "รอส่ง");
            sync_color = THEME_WARN;
        } else if (ap_mode) {
            safe_copy(sync_value, sizeof(sync_value), "ตั้งค่า");
        } else {
            safe_copy(sync_value, sizeof(sync_value), "ออฟไลน์");
            sync_color = THEME_WARN;
        }
    } else {
        if (wifi_online) {
            safe_copy(network_value, sizeof(network_value), "Online");
            safe_copy(address_value, sizeof(address_value), s_ip);
        } else if (ap_mode) {
            safe_copy(network_value, sizeof(network_value), "Setup");
            safe_copy(address_value, sizeof(address_value), "192.168.4.1");
            network_color = THEME_WARN;
        } else {
            safe_copy(network_value, sizeof(network_value), "Offline");
            safe_copy(address_value, sizeof(address_value), "-");
            network_color = THEME_WARN;
        }

        if (wifi_online) {
            safe_copy(sync_value, sizeof(sync_value), "Ready");
        } else if (pending_events > 0) {
            safe_copy(sync_value, sizeof(sync_value), "Queued");
            sync_color = THEME_WARN;
        } else if (ap_mode) {
            safe_copy(sync_value, sizeof(sync_value), "Setup");
        } else {
            safe_copy(sync_value, sizeof(sync_value), "Offline");
            sync_color = THEME_WARN;
        }
    }

    draw_card_shell(y, STATUS_CARD_H, SB_COLOR_PRIMARY);

    if (g_ui_language == UI_LANG_TH) {
        draw_utf8_text_line(CARD_X + 12, y + STATUS_TITLE_Y + THAI_TEXT_NUDGE, "สถานะระบบ", SB_COLOR_TXT_MAIN);
        draw_status_label(CARD_X + 12, y + STATUS_ROW1_Y + THAI_TEXT_NUDGE, "เครือข่าย");
        draw_status_label(CARD_X + 12, y + STATUS_ROW2_Y + THAI_TEXT_NUDGE, "ที่อยู่");
        draw_status_label(CARD_X + 12, y + STATUS_ROW3_Y + THAI_TEXT_NUDGE, "ซิงก์");
    } else {
        draw_string_gfx(CARD_X + 12, y + STATUS_TITLE_Y, "Device status", SB_COLOR_TXT_MAIN, SB_COLOR_CARD, &FreeSans9pt7b);
        draw_status_label(CARD_X + 12, y + STATUS_ROW1_Y, "Network");
        draw_status_label(CARD_X + 12, y + STATUS_ROW2_Y, "Address");
        draw_status_label(CARD_X + 12, y + STATUS_ROW3_Y, "Sync");
    }

    draw_status_value(CARD_X + 132, y + STATUS_ROW1_Y + (g_ui_language == UI_LANG_TH ? THAI_VALUE_NUDGE : 0), network_value, network_color);
    draw_status_value(CARD_X + 132, y + STATUS_ROW2_Y + (g_ui_language == UI_LANG_TH ? THAI_VALUE_NUDGE : 0), address_value, SB_COLOR_TXT_MAIN);
    draw_status_value(CARD_X + 132, y + STATUS_ROW3_Y + (g_ui_language == UI_LANG_TH ? THAI_VALUE_NUDGE : 0), sync_value, sync_color);
}

void ui_settings_render(void)
{
    if (force_redraw || !s_settings_layout_ready) {
        fill_screen(SB_COLOR_BG);
        draw_top_bar_with_back(g_ui_language == UI_LANG_TH ? NULL : "System");
        if (g_ui_language == UI_LANG_TH) {
            draw_label_bitmap((LCD_W - kThTopSettings.width) / 2, 8, &kThTopSettings);
        }
        fill_rect(0, 44, LCD_W, LCD_H - 44, SB_COLOR_BG);

        s_settings_layout_ready = true;
        s_settings_alert_dirty = true;
        s_settings_nav_dirty = true;
        s_settings_status_dirty = true;
    }

    if (s_settings_alert_dirty) {
        draw_volume_card(ALERT_Y, g_alert_volume, true);
        s_settings_alert_dirty = false;
    }

    if (s_settings_nav_dirty) {
        draw_volume_card(NAV_Y, g_nav_volume, false);
        s_settings_nav_dirty = false;
    }

    if (s_settings_status_dirty) {
        draw_system_status_card(STATUS_Y);
        s_settings_status_dirty = false;
    }

    force_redraw = false;
}

void ui_settings_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    if (tx_n >= 14 && tx_n <= 118 && ty_n >= 8 && ty_n <= 34) {
        dfplayer_play_track(g_snd_button);
        pending_page = PAGE_MENU;
        s_settings_layout_ready = false;
        return;
    }

    bool changed_alert = false;
    bool changed_nav = false;
    bool changed = false;
    int minus_x = settings_minus_x();

    if (ty_n >= ALERT_Y + BTN_ROW_Y_OFF && ty_n <= ALERT_Y + BTN_ROW_Y_OFF + BTN_H) {
        if (tx_n >= minus_x && tx_n <= minus_x + BTN_W) {
            if (g_alert_volume > 0) {
                g_alert_volume--;
                changed_alert = true;
                s_settings_alert_dirty = true;
            }
        } else if (tx_n >= settings_plus_x(true) && tx_n <= settings_plus_x(true) + BTN_W) {
            if (g_alert_volume < 30) {
                g_alert_volume++;
                changed_alert = true;
                s_settings_alert_dirty = true;
            }
        }
    }

    if (ty_n >= NAV_Y + BTN_ROW_Y_OFF && ty_n <= NAV_Y + BTN_ROW_Y_OFF + BTN_H) {
        if (tx_n >= minus_x && tx_n <= minus_x + BTN_W) {
            if (g_nav_sound_enabled && g_nav_volume > 0) {
                g_nav_volume--;
                changed_nav = true;
                s_settings_nav_dirty = true;
            }
        } else if (tx_n >= settings_plus_x(false) && tx_n <= settings_plus_x(false) + BTN_W) {
            if (g_nav_sound_enabled && g_nav_volume < 30) {
                g_nav_volume++;
                changed_nav = true;
                s_settings_nav_dirty = true;
            }
        } else if (tx_n >= settings_toggle_x() && tx_n <= settings_toggle_x() + TOGGLE_W) {
            g_nav_sound_enabled = !g_nav_sound_enabled;
            changed = true;
            s_settings_nav_dirty = true;
            if (g_nav_sound_enabled) dfplayer_play_track(35);
            else dfplayer_play_track(36);
        }
    }

    // Voice feedback for +/- presses: เพิ่ม / ลด (TH) or increase / decrease
    // (EN). Uses the dedicated track numbers stored in NVS so the technician
    // can swap them from the web panel.
    int feedback_track = 0;
    if (changed_alert || changed_nav) {
        bool is_th = (g_ui_language == UI_LANG_TH);
        // alert volume + : we know it changed if g_alert_volume just went up
        // We can't know direction from the changed_* flags above so derive
        // from which button rect was hit. Easier: check the input coords.
        bool plus_hit = (tx_n >= settings_plus_x(changed_alert) &&
                         tx_n <= settings_plus_x(changed_alert) + BTN_W);
        if (plus_hit) {
            feedback_track = is_th ? g_snd_volup_th : g_snd_volup_en;
        } else {
            feedback_track = is_th ? g_snd_voldn_th : g_snd_voldn_en;
        }
    }

    if (changed_alert) {
        settings_save_nvs();
        dfplayer_stop();
        vTaskDelay(pdMS_TO_TICKS(50));
        if (feedback_track > 0) {
            dfplayer_play_track_force_vol((uint16_t)feedback_track, g_alert_volume);
        } else {
            dfplayer_play_track_force_vol(g_snd_alarm, g_alert_volume);
        }
    } else if (changed_nav) {
        settings_save_nvs();
        dfplayer_stop();
        vTaskDelay(pdMS_TO_TICKS(50));
        if (feedback_track > 0) {
            dfplayer_play_track_force_vol((uint16_t)feedback_track, g_nav_volume);
        } else {
            dfplayer_play_track_force_vol(g_snd_button, g_nav_volume);
        }
    } else if (changed) {
        settings_save_nvs();
    }
}
