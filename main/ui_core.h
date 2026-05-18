#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "Adafruit_GFX.h"
#include "FreeSans24pt7b.h"
#include "FreeSans18pt7b.h"
#include "FreeSans12pt7b.h"
#include "FreeSans9pt7b.h"
#include "FreeSansBold24pt7b.h"
#include "FreeSansBold18pt7b.h"
#include "netpie_mqtt.h"

#define LCD_W 480
#define LCD_H 320

#define ST_RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define THEME_BG          ST_RGB565( 28,  54,  88)
#define THEME_PANEL       ST_RGB565( 42,  75, 115)
#define THEME_CARD        ST_RGB565( 50,  88, 135)
#define THEME_BORDER      ST_RGB565( 56, 189, 248)
#define THEME_ACCENT      ST_RGB565( 56, 189, 248)
#define THEME_TXT_MAIN    ST_RGB565(255, 255, 255)
#define THEME_TXT_MUTED   ST_RGB565(160, 185, 215)
#define THEME_OK          ST_RGB565( 16, 185, 129)
#define THEME_WARN        ST_RGB565(245, 158,  11)
#define THEME_BAD         ST_RGB565(239,  68,  68)
#define THEME_INACTIVE    ST_RGB565( 71,  85, 105)

#define SB_COLOR_BG        THEME_BG
#define SB_COLOR_PANEL     THEME_PANEL
#define SB_COLOR_CARD      THEME_CARD
#define SB_COLOR_BORDER    THEME_BORDER
#define SB_COLOR_PRIMARY   THEME_ACCENT
#define SB_COLOR_TXT_MAIN  THEME_TXT_MAIN
#define SB_COLOR_TXT_MUTED THEME_TXT_MUTED
#define SB_COLOR_MUTED     THEME_INACTIVE
#define SB_COLOR_ACCENT    THEME_WARN
#define SB_COLOR_OK        THEME_OK
#define SB_COLOR_OFF       THEME_INACTIVE

/* iOS-style keyboard palette (light theme):
 *   bg     — light grey backdrop          #d1d5db
 *   key    — clean white letter face      #ffffff
 *   txt    — near-black main letter       #1c1c1e
 *   sym    — secondary hint label         #6b7280
 *   space  — same white as letter         #ffffff
 *   ctrl   — slightly darker grey (shift/123/TH) #adb5bd
 *   shadow — subtle border under key      #b0b3b8
 *   ent    — iOS-system blue (return/SAVE) #007aff
 *   del    — iOS-system red (cancel/BS)   #ff3b30
 *   shift  — iOS-system orange (toggle)   #ff9500
 *   hdr    — same as bg (seamless)        #d1d5db
 *   hdr_txt — near-black                  #1c1c1e */
#define KB_COLOR_BG ST_RGB565(209, 213, 219)
#define KB_KEY     0xFFFF
#define KB_KEY_BD  ST_RGB565(176, 179, 184)
#define KB_TXT     ST_RGB565( 28,  28,  30)
#define KB_SYM     ST_RGB565(107, 114, 128)
#define KB_SPACE   0xFFFF
#define KB_ENT     ST_RGB565(  0, 122, 255)
#define KB_DEL     ST_RGB565(255,  59,  48)
#define KB_SHIFT   ST_RGB565(255, 149,   0)
#define KB_NUMPAD  ST_RGB565(173, 181, 189)
#define KB_HDR     ST_RGB565(209, 213, 219)
#define KB_HDR_TXT ST_RGB565( 28,  28,  30)

#define COLOR_BG          THEME_BG
#define COLOR_CARD        THEME_PANEL
#define COLOR_HDR         THEME_PANEL
#define COLOR_HDR_TXT     THEME_TXT_MAIN
#define COLOR_TXT         THEME_TXT_MAIN
#define COLOR_SEC         THEME_TXT_MUTED
#define COLOR_DATE        THEME_TXT_MUTED
#define COLOR_MUTED       THEME_TXT_MUTED
#define COLOR_PRI         THEME_ACCENT
#define COLOR_BTN_PRI     THEME_ACCENT
#define COLOR_ACCENT      THEME_WARN
#define COLOR_BTN_TEAL    THEME_ACCENT
#define COLOR_BTN_PINK    THEME_WARN

#define HDR               THEME_PANEL
#define CARD              THEME_CARD
#define TXT               THEME_TXT_MAIN
#define SUB               THEME_TXT_MUTED
#define KB_BG             THEME_BG

enum ui_page_t {
    PAGE_INTRO,
    PAGE_STANDBY,
    PAGE_MENU,
    PAGE_KEYBOARD,
    PAGE_SETUP_SCHEDULE,
    PAGE_SETUP_MEDS,
    PAGE_SETUP_MEDS_DETAIL,
    PAGE_MANUAL,
    PAGE_SETTINGS,
    PAGE_WIFI_SCAN,
    PAGE_WIFI_STATUS,
    PAGE_TIME_PICKER,
    PAGE_CONFIRM_MEDS
};

enum ui_language_t {
    UI_LANG_EN = 0,
    UI_LANG_TH = 1
};

extern enum ui_page_t current_page;
extern enum ui_page_t pending_page;
extern enum ui_language_t g_ui_language;
extern bool force_redraw;

extern bool s_sched_warn_dismissed;
extern bool s_hw_warn_dismissed;
extern int s_popup_state;
extern uint32_t s_netpie_sync_popup_until;
extern char s_ip[32];
extern bool s_ip_dirty;

void ui_standby_render(uint32_t now);
void ui_standby_handle_touch(uint16_t tx_n, uint16_t ty_n);
/* Tell the standby renderer the next ui_standby_render() is a fresh
 * (re-)entry — repaint the heavy gradient + card frames. clock_task
 * calls this on transitions away from standby. Touch handlers that
 * only flip popup state should NOT call this; the popup-level diff
 * paints itself without disturbing the chrome. */
void ui_standby_invalidate_chrome(void);
/* True while the unconditional boot-clear modal (popup state 7) is
 * still up — i.e. user hasn't pressed Clear yet. Dispense scheduler
 * checks this to refuse slot-time matches that would otherwise yank
 * the user off standby and bypass the lock. Implemented in
 * ui_standby.cpp; must keep C linkage so dispenser_scheduler.c (C
 * file) can call it. */
#ifdef __cplusplus
extern "C" {
#endif
bool ui_standby_boot_clear_pending(void);
#ifdef __cplusplus
}
#endif

void ui_menu_render(void);
void ui_menu_handle_touch(uint16_t tx_n, uint16_t ty_n);

void ui_setup_schedule_render(void);
void ui_setup_schedule_handle_touch(uint16_t tx_n, uint16_t ty_n);
void ui_time_picker_render(void);
void ui_time_picker_handle_touch(uint16_t tx_n, uint16_t ty_n, bool long_press);
void ui_time_picker_handle_hold(uint16_t tx_n, uint16_t ty_n);

extern int edit_slot;
extern int edit_hh;
extern int edit_mm;

extern bool is_med_name_setup;
extern int selected_med_idx;
extern int return_qty;
extern bool show_return_confirm;
extern char kb_input_buf[96];
extern bool kb_input_dirty;
extern char kb_title_buf[64];

void ui_setup_meds_render(void);
void ui_setup_meds_handle_touch(uint16_t tx_n, uint16_t ty_n);

void ui_setup_meds_detail_render(void);
void ui_setup_meds_detail_handle_touch(uint16_t tx_n, uint16_t ty_n);
/* Repeated every 70 ms while the user keeps holding +/- on the
 * pill-count buttons (initial 320 ms delay matches the time picker). */
void ui_setup_meds_detail_handle_hold(uint16_t tx_n, uint16_t ty_n);

#ifdef __cplusplus
extern "C" {
#endif
/* Called by the page-transition watcher when the user leaves the
 * meds-detail page through a path other than Save/Back (e.g. a
 * scheduled-dispense Confirm popup interrupts editing). Releases the
 * NETPIE publish inhibit and commits any pending diff. No-op if no
 * edit session is active. */
void ui_setup_meds_end_edit_session_if_any(void);

/* Arms the refill-or-clear popup on the meds-detail page. Display
 * task calls this immediately after auto-navigating to a module
 * whose scheduled dose came up empty / IR-missed. */
void ui_setup_meds_arm_refill_or_clear(void);
#ifdef __cplusplus
}
#endif

#define MAX_SCANNED_UI_APS 15
#include "esp_wifi_types.h"
extern wifi_ap_record_t scanned_aps[MAX_SCANNED_UI_APS];
extern uint16_t ap_count;
extern char selected_ssid[33];
extern bool is_wifi_setup;
extern int wifi_scroll;
extern uint8_t wf_state;
extern char s_ip[32];

void ui_wifi_scan_render(void);
void ui_wifi_scan_handle_touch(uint16_t tx_n, uint16_t ty_n);

void ui_wifi_status_render(void);
void ui_wifi_status_handle_touch(uint16_t tx_n, uint16_t ty_n);

void ui_keyboard_render(void);
void ui_keyboard_handle_touch(uint16_t tx_n, uint16_t ty_n);
void ui_keyboard_prepare(bool prefer_th);

void ui_confirm_render(void);
void ui_confirm_handle_touch(uint16_t tx_n, uint16_t ty_n);
/* BUG FIX #6: call when current_page becomes PAGE_CONFIRM_MEDS so the
 * confirm page rejects carried-over touch events from the previous page
 * for a short debounce window. See ui_confirm.cpp for details. */
#ifdef __cplusplus
extern "C" {
#endif
void ui_confirm_arm_on_enter(void);
#ifdef __cplusplus
}
#endif

// Global settings (volume + pre-alert)
extern int g_alert_volume;
extern int g_nav_volume;
extern bool g_nav_sound_enabled;         // 0-30: current DFPlayer volume
extern int g_snd_button;
// Removed g_pre_alert_minutes
#ifdef __cplusplus
extern "C" {
#endif
void settings_save_nvs(void);
void settings_load_nvs(void);
#ifdef __cplusplus
}
#endif

void ui_settings_render(void);
void ui_settings_handle_touch(uint16_t tx_n, uint16_t ty_n);

void fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

#ifdef __cplusplus
extern "C" {
#endif
void ui_draw_rgb_bitmap(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* bitmap);
#ifdef __cplusplus
}
#endif

void draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void fill_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
void fill_round_rect_frame(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t fill, uint16_t border);
void draw_gradient_v(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c1, uint16_t c2);
void fill_screen(uint16_t color);

int16_t draw_string_gfx(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg, const GFXfont *font);
int16_t gfx_text_width(const char *str, const GFXfont *font);
int16_t draw_string_gfx_scaled(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg, const GFXfont *font, uint8_t scale);
int16_t gfx_text_width_scaled(const char *str, const GFXfont *font, uint8_t scale);
void draw_string_centered(int16_t cx, int16_t baseline_y, const char *str, uint16_t fg, uint16_t bg, const GFXfont *font);

size_t safe_copy(char *dst, size_t dst_sz, const char *src);
void ui_map_touch(uint16_t raw_x, uint16_t raw_y, uint16_t *ux, uint16_t *uy);
void draw_top_bar_with_back(const char *title);

// Scaled UTF-8 drawing functions (implemented in ui_standby.cpp)
#include "ui_utf8_font_data.h"
void ui_utf8_draw_glyph_mask_scaled(int16_t x, int16_t y, const ui_utf8_font_glyph_t *glyph, uint16_t color, uint8_t target_height);
int16_t ui_utf8_text_width_scaled_px(const char *text, uint8_t target_height);
int16_t ui_utf8_draw_text_scaled_px(int16_t x, int16_t y, const char *text, uint16_t color, uint8_t target_height);
void draw_utf8_centered_line_scaled(int16_t center_x, int16_t top_y, const char *text, uint16_t fg, uint16_t bg, uint8_t target_height);
