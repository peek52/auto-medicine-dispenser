#include "ui_core.h"
#include "wifi_sta.h"
#include "dfplayer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_wifi_thai_labels.h"

static const char *TAG = "UI_WIFI";

extern volatile int ui_manual_disp_status;
static uint32_t s_scan_wait_start_ms = 0;

static void fit_ascii_text(char *dst, size_t dst_cap, const char *src, int16_t max_w, const GFXfont *font)
{
    if (!dst || dst_cap == 0) return;
    dst[0] = '\0';
    if (!src) return;

    safe_copy(dst, dst_cap, src);
    if (gfx_text_width(dst, font) <= max_w) return;

    const char *ellipsis = "...";
    size_t src_len = strlen(src);
    if (src_len == 0) return;

    for (int cut = (int)src_len - 1; cut >= 0; --cut) {
        size_t keep = (size_t)cut;
        if (keep + strlen(ellipsis) + 1 > dst_cap) keep = dst_cap - strlen(ellipsis) - 1;
        memcpy(dst, src, keep);
        dst[keep] = '\0';
        strncat(dst, ellipsis, dst_cap - strlen(dst) - 1);
        if (gfx_text_width(dst, font) <= max_w) return;
    }

    safe_copy(dst, dst_cap, ellipsis);
}

static void draw_wifi_label(int16_t x, int16_t y, const ui_label_bitmap_t *label)
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
                          src[col] != SB_COLOR_PRIMARY);
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

void ui_wifi_scan_render(void)
{
    // 3-state machine: 0=show scanning UI & start scan, 1=wait for results, 2=draw results

    if (wf_state == 0) {
        s_scan_wait_start_ms = 0;
        // ── Pass 1: Draw scanning card and START scan ──
        if (force_redraw) {
            fill_screen(THEME_BG);
            draw_top_bar_with_back(g_ui_language == UI_LANG_TH ? NULL : "WIFI SETUP");
            if (g_ui_language == UI_LANG_TH) {
                draw_wifi_label((LCD_W - kThTopWifiSetup.width) / 2, 8, &kThTopWifiSetup);
            }

            fill_rect(60, 68, 360, 200, SB_COLOR_CARD);
            draw_rect(60, 68, 360, 200, SB_COLOR_BORDER);
            fill_rect(220, 100, 40, 4, SB_COLOR_PRIMARY);
            fill_rect(200, 116, 80, 4, SB_COLOR_PRIMARY);
            fill_rect(180, 132, 120, 4, SB_COLOR_PRIMARY);
            fill_rect(236, 150, 8, 8, SB_COLOR_PRIMARY);
            if (g_ui_language == UI_LANG_TH) {
                draw_wifi_label((LCD_W - kThScanningWifi.width) / 2, 176, &kThScanningWifi);
                draw_wifi_label((LCD_W - kThSearchingWifi.width) / 2, 214, &kThSearchingWifi);
            } else {
                draw_string_centered(LCD_W / 2, 198, "SCANNING WIFI", SB_COLOR_TXT_MAIN, SB_COLOR_CARD, &FreeSans18pt7b);
                draw_string_centered(LCD_W / 2, 230, "Searching for networks...", SB_COLOR_TXT_MUTED, SB_COLOR_CARD, &FreeSans9pt7b);
            }
        
            // Start Async Scan!
            wifi_sta_start_scan();
        }

        wf_state = 1; // trigger pass 2 on NEXT loop iteration
        force_redraw = false; // consume external redraw

    } else if (wf_state == 1) {
        // ── Pass 2: Polling for scan results (non-blocking) ──
        if (s_scan_wait_start_ms == 0) s_scan_wait_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        int result = wifi_sta_get_scan_results(scanned_aps, MAX_SCANNED_UI_APS);
        uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_scan_wait_start_ms;

        if (result >= 0) {
            ap_count = result;
            s_scan_wait_start_ms = 0;
            wifi_scroll = 0;
            
            // Note: In original code, these static vars from clock_task were touched here:
            // prev_touch = false; last_tx = 0; last_ty = 0;
            // touch_handled = true;
            // Since they are static to clock_task, we will handle the transition back in display_clock properly.
            // However, resetting wf_state to 2 and force_redraw = true allows the loop to recover safely.
            
            wf_state = 2;
            force_redraw = true; // Schedule drawing pass 3
        } else if (elapsed > 7500) {
            // Timeout explicitly
            ESP_LOGW(TAG, "Scan timeout from UI side");
            ap_count = 0;
            s_scan_wait_start_ms = 0;
            wf_state = 2;
            force_redraw = true; // Still go to pass 3 to clear the loading screen
        }

    } else if (wf_state == 2) {
        // ── Pass 3: Draw results ──
        if (force_redraw) {
            fill_screen(SB_COLOR_BG);
            fill_rect(0, 0, LCD_W, 44, SB_COLOR_PANEL);
            fill_rect(0, 43, LCD_W, 2, SB_COLOR_PRIMARY);
            fill_rect(10, 8, 80, 26, SB_COLOR_PRIMARY);
            draw_rect(10, 8, 80, 26, SB_COLOR_BORDER);
            draw_string_gfx(22, 26, "BACK", 0x0000, SB_COLOR_PRIMARY, &FreeSans9pt7b);
            if (g_ui_language != UI_LANG_TH) {
                draw_string_centered(LCD_W / 2, 30, "WIFI SETUP", SB_COLOR_TXT_MAIN, SB_COLOR_PANEL, &FreeSans18pt7b);
            }
            if (g_ui_language == UI_LANG_TH) {
                draw_wifi_label((LCD_W - kThTopWifiSetup.width) / 2, 8, &kThTopWifiSetup);
            }
            
            // Bottom action bar
            fill_rect(0, 278, LCD_W, 42, SB_COLOR_PANEL);
            fill_rect(0, 278, LCD_W, 1, SB_COLOR_BORDER);
            fill_rect(10, 284, 220, 30, THEME_BAD);
            draw_rect(10, 284, 220, 30, SB_COLOR_BORDER);
            if (g_ui_language == UI_LANG_TH) {
                draw_wifi_label(10 + ((220 - kThForgetWifi.width) / 2), 289, &kThForgetWifi);
            } else {
                draw_string_centered(120, 305, "FORGET", 0xFFFF, THEME_BAD, &FreeSans12pt7b);
            }
            fill_rect(250, 284, 220, 30, SB_COLOR_PRIMARY);
            draw_rect(250, 284, 220, 30, SB_COLOR_BORDER);
            if (g_ui_language == UI_LANG_TH) {
                draw_wifi_label(250 + ((220 - kThRescan.width) / 2), 289, &kThRescan);
            } else {
                draw_string_centered(360, 305, "RESCAN", 0x0000, SB_COLOR_PRIMARY, &FreeSans12pt7b);
            }
            
            force_redraw = false;
        }

        static int prev_scroll = -1;
        static int prev_ap_count = -1;
        if (wifi_scroll != prev_scroll || ap_count != prev_ap_count) {
            prev_scroll = wifi_scroll;
            prev_ap_count = ap_count;

            const int VISIBLE = 4;
            if (ap_count > 0) {
                fill_rect(0, 44, LCD_W, 234, SB_COLOR_BG); // Clear List Area completely
                fill_rect(0, 44, LCD_W, 22, SB_COLOR_PANEL);
            if (g_ui_language == UI_LANG_TH) {
                draw_wifi_label(16, 48, &kThSsid);
                draw_wifi_label(350, 48, &kThSignal);
            } else {
                draw_string_gfx(16, 60, "SSID", SB_COLOR_TXT_MUTED, SB_COLOR_PANEL, &FreeSans9pt7b);
                draw_string_gfx(360, 60, "SIG", SB_COLOR_TXT_MUTED, SB_COLOR_PANEL, &FreeSans9pt7b);
            }

            for (int i = 0; i < VISIBLE; i++) {
                int idx = wifi_scroll + i;
                if (idx >= ap_count) break;
                int cy = 66 + i * 52;
                uint16_t cbg = (i % 2 == 0) ? SB_COLOR_CARD : SB_COLOR_BG;
                fill_rect(0, cy, LCD_W, 51, cbg);
                fill_rect(0, cy, 5, 51, SB_COLOR_PRIMARY);

                char ssid_safe[20] = "";
                const uint8_t *raw = scanned_aps[idx].ssid;
                int slen = 0;
                for (int s = 0; s < 32 && raw[s] && slen < 18; s++)
                    ssid_safe[slen++] = (raw[s] >= 0x20 && raw[s] <= 0x7E) ? (char)raw[s] : '?';
                ssid_safe[slen] = '\0';
                if (slen == 0) strcpy(ssid_safe, "???");

                draw_string_gfx(16, cy + 33, ssid_safe, SB_COLOR_TXT_MAIN, cbg, &FreeSans12pt7b);

                int rssi = scanned_aps[idx].rssi;
                const char *sig = (rssi > -55) ? "GREAT" : (rssi > -65) ? "GOOD" : (rssi > -75) ? "FAIR" : "WEAK";
                uint16_t sc  = (rssi > -55) ? THEME_OK : (rssi > -65) ? THEME_OK : (rssi > -75) ? 0xFEA0 : THEME_BAD;
                draw_string_gfx(360, cy + 33, sig, sc, cbg, &FreeSans9pt7b);
                fill_rect(0, cy + 50, LCD_W, 1, SB_COLOR_BORDER);
            }

            if (ap_count > VISIBLE) {
                fill_rect(380, 8, 40, 26, SB_COLOR_CARD);
                draw_rect(380, 8, 40, 26, SB_COLOR_BORDER);
                draw_string_centered(400, 26, "UP", SB_COLOR_PRIMARY, SB_COLOR_CARD, &FreeSans9pt7b);
                fill_rect(424, 8, 40, 26, SB_COLOR_CARD);
                draw_rect(424, 8, 40, 26, SB_COLOR_BORDER);
                draw_string_centered(444, 26, "DN", SB_COLOR_PRIMARY, SB_COLOR_CARD, &FreeSans9pt7b);
            }
            }
        }
    }
}

void ui_wifi_scan_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    static uint32_t last_scan_ms = 0;
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // BACK — top-left
    if (tx_n >= 10 && tx_n <= 90 && ty_n >= 8 && ty_n <= 34) {
        dfplayer_play_track(10); // Track 10: Back button audio
        pending_page = PAGE_MENU;
    }
    // Scroll UP
    else if (tx_n >= 380 && tx_n <= 420 && ty_n >= 8 && ty_n <= 34) {
        if (wifi_scroll > 0) wifi_scroll--;
    }
    // Scroll DN
    else if (tx_n >= 424 && tx_n <= 464 && ty_n >= 8 && ty_n <= 34) {
        if (wifi_scroll + 4 < ap_count) wifi_scroll++;
    }
    // Network row tap → go to keyboard for password
    else if (ty_n >= 66 && ty_n <= 275) {
        int row = (ty_n - 66) / 52;
        int idx = wifi_scroll + row;
        if (idx >= 0 && idx < ap_count) {
            char ssid_buf[33] = "";
            const uint8_t *raw = scanned_aps[idx].ssid;
            int sl = 0;
            for (int s = 0; s < 32 && raw[s]; s++)
                ssid_buf[sl++] = (raw[s] >= 0x20 && raw[s] <= 0x7E) ? (char)raw[s] : '?';
            ssid_buf[sl] = '\0';
            safe_copy(selected_ssid, sizeof(selected_ssid), ssid_buf);
            is_wifi_setup = true;
            
            snprintf(kb_title_buf, sizeof(kb_title_buf), "Password for %s:", selected_ssid);
            kb_input_buf[0] = '\0';
            kb_input_dirty = true;
            ui_keyboard_prepare(false);
            
            pending_page = PAGE_KEYBOARD;
            force_redraw = true;
        }
    }
    // FORGET
    else if (tx_n >= 10 && tx_n <= 230 && ty_n >= 284 && ty_n <= 314) {
        if (g_nav_sound_enabled) dfplayer_play_track(38); // Swapped in FAT: Forget WiFi (now 38)
        wifi_sta_forget();
        pending_page = PAGE_STANDBY;
        force_redraw = true;
    }
    // RESCAN — cooldown 15s to prevent RPC deadlock
    else if (tx_n >= 250 && tx_n <= 470 && ty_n >= 284 && ty_n <= 314) {
        if (g_nav_sound_enabled) dfplayer_play_track(37); // Swapped in FAT: Rescan WiFi (now 37)
        if (now_ms - last_scan_ms >= 15000) {
            last_scan_ms  = now_ms;
            wifi_scroll   = 0;
            wf_state      = 0; // Reset scan state!
            s_scan_wait_start_ms = 0;
            force_redraw  = true;
        }
    }
}

void ui_wifi_status_render(void)
{
    if (force_redraw) {
        fill_screen(THEME_BG);
        draw_top_bar_with_back(g_ui_language == UI_LANG_TH ? NULL : "WiFi Status");
        if (g_ui_language == UI_LANG_TH) {
            draw_wifi_label((LCD_W - kThWifiStatus.width) / 2, 10, &kThWifiStatus);
        }

        fill_round_rect(20, 72, LCD_W - 40, 188, 12, CARD);

        if (g_ui_language == UI_LANG_TH) {
            draw_wifi_label(36, 92, &kThConnectedSsid);
        } else {
            draw_string_gfx(36, 108, "Connected SSID", SUB, CARD, &FreeSans9pt7b);
        }

        wifi_config_t conf;
        char ssid_line[40] = "Not connected";
        if (wifi_sta_connected()) {
            if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
                safe_copy(ssid_line, sizeof(ssid_line), (char*)conf.sta.ssid);
                if (strlen(ssid_line) == 0) safe_copy(ssid_line, sizeof(ssid_line), "Not connected");
            }
        }

        char ssid_fit[40];
        fit_ascii_text(ssid_fit, sizeof(ssid_fit), ssid_line, LCD_W - 92, &FreeSans18pt7b);
        draw_string_gfx(36, 136, ssid_fit, TXT, CARD, &FreeSans18pt7b);

        draw_string_gfx(36, 168, "Service Login", SUB, CARD, &FreeSans9pt7b);

        char root_url[64] = "Not available";
        if (strcmp(s_ip, "0.0.0.0") != 0) {
            snprintf(root_url, sizeof(root_url), "http://%s", s_ip);
        }

        char root_fit[64];
        fit_ascii_text(root_fit, sizeof(root_fit), root_url, LCD_W - 92, &FreeSans9pt7b);
        draw_string_gfx(36, 183, root_fit, 0x1A7B, CARD, &FreeSans9pt7b);

        draw_string_gfx(36, 205, "Tech Dashboard", SUB, CARD, &FreeSans9pt7b);
        draw_string_gfx(214, 205, "/maint", 0x1A7B, CARD, &FreeSans9pt7b);

        draw_string_gfx(36, 226, "WiFi Setup", SUB, CARD, &FreeSans9pt7b);
        draw_string_gfx(214, 226, "/wifi", 0x1A7B, CARD, &FreeSans9pt7b);

        draw_string_gfx(36, 247, "Cloud Config", SUB, CARD, &FreeSans9pt7b);
        draw_string_gfx(214, 247, "/cloud", 0x1A7B, CARD, &FreeSans9pt7b);

        fill_round_rect(20, 268, LCD_W - 40, 44, 12, THEME_BAD);
        if (g_ui_language == UI_LANG_TH) {
            draw_wifi_label((LCD_W - kThForgetWifi.width) / 2, 277, &kThForgetWifi);
        } else {
            draw_string_centered(LCD_W / 2, 298, "Forget WiFi", 0xFFFF, THEME_BAD, &FreeSans12pt7b);
        }
        
        force_redraw = false;
    }
}

void ui_wifi_status_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    if (tx_n >= 10 && tx_n <= 90 && ty_n >= 8 && ty_n <= 34) {
        dfplayer_play_track(10);
        pending_page = PAGE_STANDBY;
    } else if (tx_n >= 20 && tx_n <= 460 && ty_n >= 268 && ty_n <= 312) {
        if (g_nav_sound_enabled) dfplayer_play_track(38); // Swapped in FAT: Forget WiFi (now 38)
        wifi_sta_forget();
        pending_page = PAGE_STANDBY;
        force_redraw = true;
    }
}
