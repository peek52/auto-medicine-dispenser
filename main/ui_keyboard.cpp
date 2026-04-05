#include "ui_core.h"
#include "esp_log.h"
#include "wifi_sta.h"
#include "netpie_mqtt.h"
#include "ui_utf8_text.h"
#include "ui_keyboard_thai_labels.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "UI_KEYBOARD";

typedef struct {
    char letter;
    char number;
    int16_t x, y, w, h;
} kb_key_t;

typedef struct {
    const char *label;
    int16_t x, y, w, h;
} kb_th_key_t;

static const kb_key_t kb_keys[] = {
    {'q','1',  0, 70,48,58},{'w','2', 48, 70,48,58},{'e','3', 96, 70,48,58},{'r','4',144, 70,48,58},{'t','5',192, 70,48,58},
    {'y','6',240, 70,48,58},{'u','7',288, 70,48,58},{'i','8',336, 70,48,58},{'o','9',384, 70,48,58},{'p','0',432, 70,48,58},

    {'a','+', 24,132,48,58},{'s','-', 72,132,48,58},{'d','*',120,132,48,58},{'f','/',168,132,48,58},{'g','(',216,132,48,58},
    {'h',')',264,132,48,58},{'j','@',312,132,48,58},{'k','.',360,132,48,58},{'l','!',408,132,48,58},

    {'^','^',  0,194,68,58},
    {'z',',', 68,194,46,58},{'x','?',114,194,46,58},{'c',';',160,194,46,58},{'v',':',206,194,46,58},
    {'b','\'',252,194,46,58},{'n','"',298,194,46,58},{'m','_',344,194,46,58},
    {'\b','\b',390,194,90,58},

    {'~','~',  0,256,64,58},
    {'#','#', 64,256,64,58},
    {' ',' ',128,256,184,58},
    {'\n','\n',312,256,168,58}
};

static const kb_th_key_t kb_th_page0[] = {
    {"โ",   0,  70, 48, 56},{"ไ",  48,  70, 48, 56},{"ำ",  96,  70, 48, 56},{"พ", 144,  70, 48, 56},{"ะ", 192,  70, 48, 56},
    {"ั", 240,  70, 48, 56},{"ี", 288,  70, 48, 56},{"ร", 336,  70, 48, 56},{"น", 384,  70, 48, 56},{"ย", 432,  70, 48, 56},
    {"บ",   0, 128, 48, 56},{"ล",  48, 128, 48, 56},{"ฟ",  96, 128, 48, 56},{"ห", 144, 128, 48, 56},{"ก", 192, 128, 48, 56},
    {"ด", 240, 128, 48, 56},{"เ", 288, 128, 48, 56},{"้", 336, 128, 48, 56},{"่", 384, 128, 48, 56},{"า", 432, 128, 48, 56},
    {"ส",   0, 186, 48, 56},{"ว",  48, 186, 48, 56},{"ง",  96, 186, 48, 56},{"ผ", 144, 186, 48, 56},{"ป", 192, 186, 48, 56},
    {"แ", 240, 186, 48, 56},{"อ", 288, 186, 48, 56},{"ิ", 336, 186, 48, 56},{"ื", 384, 186, 48, 56},{"ท", 432, 186, 48, 56}
};

static const kb_th_key_t kb_th_page1[] = {
    {"ม",   0,  70, 48, 56},{"ใ",  48,  70, 48, 56},{"ช",  96,  70, 48, 56},{"ข", 144,  70, 48, 56},{"ค", 192,  70, 48, 56},
    {"ต", 240,  70, 48, 56},{"จ", 288,  70, 48, 56},{"ถ", 336,  70, 48, 56},{"ซ", 384,  70, 48, 56},{"ญ", 432,  70, 48, 56},
    {"ุ",   0, 128, 48, 56},{"ู",  48, 128, 48, 56},{"ึ",  96, 128, 48, 56},{"็", 144, 128, 48, 56},{"๊", 192, 128, 48, 56},
    {"๋", 240, 128, 48, 56},{"์", 288, 128, 48, 56},{"ๆ", 336, 128, 48, 56},{"ภ", 384, 128, 48, 56},{"ธ", 432, 128, 48, 56},
    {"ฤ",   0, 186, 48, 56},{"ฆ",  48, 186, 48, 56},{"ฉ",  96, 186, 48, 56},{"ฐ", 144, 186, 48, 56},{"ฑ", 192, 186, 48, 56},
    {"ฒ", 240, 186, 48, 56},{"ณ", 288, 186, 48, 56},{"ศ", 336, 186, 48, 56},{"ษ", 384, 186, 48, 56},{"ฮ", 432, 186, 48, 56}
};

static const kb_th_key_t kb_th_controls[] = {
    {"P2",     0, 248, 54, 56},
    {"EN",    54, 248, 54, 56},
    {"123",  108, 248, 60, 56},
    {"SPACE",168, 248,160, 56},
    {"ลบ",   336, 248, 68, 56},
    {"SAVE", 408, 248, 72, 56}
};

char kb_input_buf[96] = "";
bool kb_input_dirty = false;
char kb_title_buf[64] = "Input:";
static bool kb_shift = false;
static bool kb_num_mode = false;
static bool kb_th_mode = false;
static uint8_t kb_th_page = 0;

static bool kb_can_use_th(void)
{
    return true;
}

static bool kb_is_med_name_th(void)
{
    return is_med_name_setup && g_ui_language == UI_LANG_TH;
}

static void draw_keyboard_bitmap_label(int16_t x, int16_t y, const ui_label_bitmap_t *label)
{
    if (!label || !label->pixels) return;
    uint16_t transparent = label->pixels[0];
    for (int16_t row = 0; row < (int16_t)label->height; ++row) {
        const uint16_t *src = label->pixels + (row * label->width);
        int16_t run_start = -1;
        for (int16_t col = 0; col <= (int16_t)label->width; ++col) {
            bool opaque = false;
            if (col < (int16_t)label->width) opaque = (src[col] != transparent);
            if (opaque && run_start < 0) run_start = col;
            else if (!opaque && run_start >= 0) {
                ui_draw_rgb_bitmap(x + run_start, y + row, col - run_start, 1, src + run_start);
                run_start = -1;
            }
        }
    }
}

static void keyboard_reset_modes(void)
{
    kb_shift = false;
    kb_num_mode = false;
    kb_th_page = 0;
    kb_th_mode = kb_can_use_th();
}

void ui_keyboard_prepare(bool prefer_th)
{
    keyboard_reset_modes();
    if (prefer_th && kb_can_use_th()) kb_th_mode = true;
}

static void keyboard_store_med_name(void)
{
    char publish_name[32];
    ui_utf8_safe_truncate_copy(publish_name, sizeof(publish_name), kb_input_buf);
    netpie_shadow_update_med_name(edit_slot + 1, publish_name);
}

static void keyboard_submit(void)
{
    ESP_LOGI(TAG, "Keyboard Submitted: %s", kb_input_buf);

    if (is_wifi_setup) {
        fill_screen(0xFFFF);
        fill_rect(0, 130, LCD_W, 50, THEME_PANEL);
        draw_string_gfx(20, 162, "Connecting to WiFi...", 0xFFFF, THEME_PANEL, &FreeSans18pt7b);
        wifi_sta_reconnect(selected_ssid, kb_input_buf);
        pending_page = PAGE_STANDBY;
        is_wifi_setup = false;
        force_redraw = true;
    }
    else if (is_med_name_setup) {
        keyboard_store_med_name();
        pending_page = PAGE_SETUP_MEDS_DETAIL;
        is_med_name_setup = false;
        force_redraw = true;
    }
    else {
        pending_page = PAGE_STANDBY;
        force_redraw = true;
    }
}

static void draw_keyboard_input_box(void)
{
    fill_round_rect_frame(8, 38, 380, 30, 6, 0xFFFF, 0xFFFF);
    if (kb_can_use_th() || ui_utf8_has_non_ascii(kb_input_buf)) {
        fill_rect(16, 42, 364, 22, 0xFFFF);
        ui_utf8_draw_text(16, 41, kb_input_buf, 0x0000);
    } else {
        draw_string_gfx(16, 60, kb_input_buf, 0x0000, 0xFFFF, &FreeSans12pt7b);
    }
}

static void draw_th_key_label(const char *label, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t fg, uint16_t bg)
{
    if (ui_utf8_has_non_ascii(label)) {
        int16_t text_w = ui_utf8_text_width(label);
        int16_t text_x = x + ((w - text_w) / 2);
        int16_t text_y = y + ((h - kUiUtf8FontLineHeight) / 2) + 1;
        fill_rect(text_x, text_y, text_w, kUiUtf8FontLineHeight, bg);
        ui_utf8_draw_text(text_x, text_y, label, fg);
    } else {
        draw_string_centered(x + (w / 2), y + 34, label, fg, bg, &FreeSans9pt7b);
    }
}

static void draw_keyboard_th_title_text(const char *text, int16_t y, uint16_t color)
{
    if (!text || !text[0]) return;
    int16_t text_w = ui_utf8_text_width(text);
    int16_t text_x = (LCD_W - text_w) / 2;
    if (text_x < 100) text_x = 100;
    ui_utf8_draw_text(text_x, y, text, color);
}

static void ui_keyboard_render_th(void)
{
    if (force_redraw) {
        fill_screen(KB_COLOR_BG);
        fill_rect(0, 0, LCD_W, 70, KB_HDR);

        fill_round_rect_frame(8, 6, 90, 28, 6, KB_DEL, 0xFFFF);
        draw_keyboard_bitmap_label(8 + ((90 - kThKbCancel.width) / 2), 6 + ((28 - kThKbCancel.height) / 2), &kThKbCancel);

        if (kb_is_med_name_th()) {
            draw_keyboard_th_title_text("ตั้งชื่อยา", 8, 0xFFFF);
        } else {
            draw_string_centered(LCD_W / 2, 25, kb_title_buf, 0xFFFF, KB_HDR, &FreeSans9pt7b);
        }

        fill_round_rect_frame(392, 38, 80, 30, 6, KB_ENT, 0xFFFF);
        draw_keyboard_bitmap_label(392 + ((80 - kThKbSave.width) / 2), 38 + ((30 - kThKbSave.height) / 2), &kThKbSave);
        draw_keyboard_input_box();

        const kb_th_key_t *page = kb_th_page == 0 ? kb_th_page0 : kb_th_page1;
        int page_count = (int)(sizeof(kb_th_page0) / sizeof(kb_th_page0[0]));

        for (int i = 0; i < page_count; ++i) {
            const kb_th_key_t *k = &page[i];
            fill_rect(k->x + 1, k->y + 1, k->w - 2, k->h - 2, KB_KEY);
            fill_rect(k->x + 1, k->y + k->h - 3, k->w - 2, 3, KB_KEY_BD);
            draw_th_key_label(k->label, k->x, k->y, k->w, k->h, KB_TXT, KB_KEY);
        }

        for (int i = 0; i < (int)(sizeof(kb_th_controls) / sizeof(kb_th_controls[0])); ++i) {
            const kb_th_key_t *k = &kb_th_controls[i];
            uint16_t face = KB_NUMPAD;
            uint16_t txt = 0xFFFF;
            if (i == 3) { face = KB_SPACE; txt = KB_TXT; }
            else if (i == 4) { face = KB_DEL; }
            else if (i == 5) { face = KB_ENT; }

            fill_rect(k->x + 1, k->y + 1, k->w - 2, k->h - 2, face);
            if (i == 3) fill_rect(k->x + 1, k->y + k->h - 3, k->w - 2, 3, KB_KEY_BD);

            if (i == 0) draw_th_key_label(kb_th_page == 0 ? "P2" : "P1", k->x, k->y, k->w, k->h, txt, face);
            else if (i == 3) draw_keyboard_bitmap_label(k->x + ((k->w - kThKbSpace.width) / 2), k->y + ((k->h - kThKbSpace.height) / 2), &kThKbSpace);
            else if (i == 4) draw_keyboard_bitmap_label(k->x + ((k->w - kThKbDelete.width) / 2), k->y + ((k->h - kThKbDelete.height) / 2), &kThKbDelete);
            else draw_th_key_label(k->label, k->x, k->y, k->w, k->h, txt, face);
        }

        kb_input_dirty = false;
    }

    if (kb_input_dirty) {
        draw_keyboard_input_box();
        kb_input_dirty = false;
    }
}

static void ui_keyboard_render_ascii(void)
{
    if (force_redraw) {
        fill_screen(KB_COLOR_BG);
        fill_rect(0, 0, LCD_W, 70, KB_HDR);

        fill_round_rect_frame(8, 6, 90, 28, 6, KB_DEL, 0xFFFF);
        draw_string_centered(53, 25, "CANCEL", 0xFFFF, KB_DEL, &FreeSans9pt7b);

        draw_string_centered(LCD_W/2, 25, kb_title_buf, 0xFFFF, KB_HDR, &FreeSans9pt7b);

        draw_keyboard_input_box();

        fill_round_rect_frame(392, 38, 80, 30, 6, KB_ENT, 0xFFFF);
        draw_string_centered(432, 59, "SAVE", 0xFFFF, KB_ENT, &FreeSans9pt7b);

        for (int i = 0; i < (int)(sizeof(kb_keys) / sizeof(kb_key_t)); i++) {
            const kb_key_t *k = &kb_keys[i];
            uint16_t face, txt;

            if (k->letter == '\n')      { face = KB_ENT; txt = KB_HDR_TXT; }
            else if (k->letter == '\b') { face = KB_DEL; txt = 0xFFFF; }
            else if (k->letter == '^')  { face = kb_shift ? KB_ENT : KB_SHIFT; txt = 0xFFFF; }
            else if (k->letter == '~' || k->letter == '#')  { face = KB_NUMPAD; txt = 0xFFFF; }
            else if (k->letter == ' ')  { face = KB_SPACE; txt = KB_TXT; }
            else                        { face = KB_KEY; txt = KB_TXT; }

            fill_rect(k->x + 1, k->y + 1, k->w - 2, k->h - 2, face);

            if (face == KB_KEY || face == KB_SPACE) {
                fill_rect(k->x + 1, k->y + k->h - 3, k->w - 2, 3, KB_KEY_BD);
            }

            if (k->letter == '\b') {
                draw_string_gfx(k->x + 6, k->y + 36, "<X", txt, face, &FreeSans12pt7b);
            } else if (k->letter == '^') {
                draw_string_gfx(k->x + 10, k->y + 36, "shift", txt, face, &FreeSans9pt7b);
            } else if (k->letter == '~') {
                draw_string_gfx(k->x + 16, k->y + 36, "TH", txt, face, &FreeSans9pt7b);
            } else if (k->letter == '#') {
                draw_string_gfx(k->x + 10, k->y + 36, (kb_num_mode ? "ABC" : "123"), txt, face, &FreeSans9pt7b);
            } else if (k->letter == ' ') {
                draw_string_gfx(k->x + 52, k->y + 36, "space", KB_SYM, face, &FreeSans12pt7b);
            } else if (k->letter == '\n') {
                draw_string_gfx(k->x + 50, k->y + 36, "return", txt, face, &FreeSans9pt7b);
            } else {
                char main_ch;
                if (kb_num_mode) main_ch = k->number;
                else main_ch = kb_shift ? (char)toupper((unsigned char)k->letter) : k->letter;

                char lbl[2] = {main_ch, '\0'};
                draw_string_gfx(k->x + 14, k->y + 38, lbl, txt, face, &FreeSans12pt7b);

                char hint = kb_num_mode ? k->letter : k->number;
                char hint_str[2] = {hint, '\0'};
                draw_string_gfx(k->x + 33, k->y + 16, hint_str, KB_SYM, face, &FreeSans9pt7b);
            }
        }
        kb_input_dirty = false;
    }

    if (kb_input_dirty) {
        fill_rect(16, 42, 364, 22, 0xFFFF);
        if (ui_utf8_has_non_ascii(kb_input_buf)) ui_utf8_draw_text(16, 41, kb_input_buf, 0x0000);
        else draw_string_gfx(16, 60, kb_input_buf, 0x0000, 0xFFFF, &FreeSans12pt7b);
        kb_input_dirty = false;
    }
}

void ui_keyboard_render(void)
{
    if (kb_can_use_th() && kb_th_mode) ui_keyboard_render_th();
    else ui_keyboard_render_ascii();
}

static void keyboard_cancel(void)
{
    pending_page = is_wifi_setup ? PAGE_WIFI_SCAN : (is_med_name_setup ? PAGE_SETUP_MEDS_DETAIL : PAGE_MENU);
    is_wifi_setup = false;
    is_med_name_setup = false;
    kb_th_mode = false;
    ESP_LOGI(TAG, "Keyboard Cancelled.");
}

static void keyboard_handle_th_touch(uint16_t tx_n, uint16_t ty_n)
{
    bool tap_cancel = (tx_n >= 8 && tx_n <= 98 && ty_n >= 6 && ty_n <= 34);
    bool tap_save   = (tx_n >= 392 && tx_n <= 472 && ty_n >= 38 && ty_n <= 68);
    if (tap_save) {
        keyboard_submit();
        return;
    }
    if (tap_cancel) {
        keyboard_cancel();
        return;
    }

    const kb_th_key_t *page = kb_th_page == 0 ? kb_th_page0 : kb_th_page1;
    int page_count = (int)(sizeof(kb_th_page0) / sizeof(kb_th_page0[0]));
    for (int i = 0; i < page_count; ++i) {
        const kb_th_key_t *k = &page[i];
        if (tx_n >= (uint16_t)k->x && tx_n <= (uint16_t)(k->x + k->w) &&
            ty_n >= (uint16_t)k->y && ty_n <= (uint16_t)(k->y + k->h)) {
            if (ui_utf8_append(kb_input_buf, sizeof(kb_input_buf), k->label, 31)) {
                kb_input_dirty = true;
            }
            return;
        }
    }

    for (int i = 0; i < (int)(sizeof(kb_th_controls) / sizeof(kb_th_controls[0])); ++i) {
        const kb_th_key_t *k = &kb_th_controls[i];
        if (tx_n >= (uint16_t)k->x && tx_n <= (uint16_t)(k->x + k->w) &&
            ty_n >= (uint16_t)k->y && ty_n <= (uint16_t)(k->y + k->h)) {
            if (i == 0) {
                kb_th_page = kb_th_page ? 0 : 1;
                force_redraw = true;
            } else if (i == 1) {
                kb_th_mode = false;
                kb_num_mode = false;
                kb_shift = false;
                force_redraw = true;
            } else if (i == 2) {
                kb_th_mode = false;
                kb_num_mode = true;
                kb_shift = false;
                force_redraw = true;
            } else if (i == 3) {
                if (ui_utf8_append(kb_input_buf, sizeof(kb_input_buf), " ", 31)) kb_input_dirty = true;
            } else if (i == 4) {
                if (ui_utf8_backspace(kb_input_buf)) kb_input_dirty = true;
            } else if (i == 5) {
                keyboard_submit();
                return;
            }
            return;
        }
    }
}

void ui_keyboard_handle_touch(uint16_t tx_n, uint16_t ty_n)
{
    if (kb_can_use_th() && kb_th_mode) {
        keyboard_handle_th_touch(tx_n, ty_n);
        return;
    }

    bool tap_cancel = (tx_n >= 8 && tx_n <= 98 && ty_n >= 6 && ty_n <= 34);
    bool tap_save   = (tx_n >= 392 && tx_n <= 472 && ty_n >= 38 && ty_n <= 68);

    if (tap_save) {
        keyboard_submit();
    } else if (tap_cancel) {
        keyboard_cancel();
    } else if (ty_n >= 70) {
        for (int i = 0; i < (int)(sizeof(kb_keys) / sizeof(kb_key_t)); i++) {
            const kb_key_t *k = &kb_keys[i];
            if (tx_n >= (uint16_t)k->x && tx_n <= (uint16_t)(k->x + k->w) &&
                ty_n >= (uint16_t)k->y && ty_n <= (uint16_t)(k->y + k->h)) {

                if (k->letter == '^') {
                    kb_shift = !kb_shift;
                    kb_num_mode = false;
                    force_redraw = true;
                } else if (k->letter == '~') {
                    kb_th_mode = true;
                    kb_num_mode = false;
                    kb_shift = false;
                    force_redraw = true;
                } else if (k->letter == '#') {
                    kb_th_mode = false;
                    kb_num_mode = !kb_num_mode;
                    kb_shift = false;
                    force_redraw = true;
                } else if (k->letter == '\b') {
                    if (ui_utf8_backspace(kb_input_buf)) kb_input_dirty = true;
                } else if (k->letter == '\n') {
                    keyboard_submit();
                } else if (k->letter == ' ') {
                    size_t max_len = is_med_name_setup ? 31 : 31;
                    if (ui_utf8_append(kb_input_buf, sizeof(kb_input_buf), " ", max_len)) kb_input_dirty = true;
                } else {
                    char ch;
                    if (kb_num_mode) ch = k->number;
                    else ch = kb_shift ? (char)toupper((unsigned char)k->letter) : k->letter;

                    char text[2] = { ch, '\0' };
                    size_t max_len = is_med_name_setup ? 31 : 31;
                    if (ui_utf8_append(kb_input_buf, sizeof(kb_input_buf), text, max_len)) {
                        kb_input_dirty = true;
                        if (kb_shift && !kb_num_mode) {
                            kb_shift = false;
                            force_redraw = true;
                        }
                    }
                }
                break;
            }
        }
    }
}
