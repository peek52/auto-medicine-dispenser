/**
 * display_clock.cpp — ST7796S 480×320 Interactive UI
 * ปรับปรุงให้หน้าจอดูนุ่มขึ้น อ่านง่ายขึ้น และลดอาการภาพทับ
 */
#include "display_clock.h"
#include "ui_core.h"
#include "config.h"
#include "ds3231.h"
#include "i2c_manager.h"
#include "dispenser_scheduler.h"
#include "usb_mouse.h"
#include "dfplayer.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include <ctype.h>
#include <math.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "wifi_sta.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "netpie_mqtt.h"
#include "dispenser_scheduler.h"
#include "idle_music.h"

static const char *TAG = "display_clk";

/* ── Pin aliases ── */
#define PIN_MOSI   TFT_MOSI
#define PIN_SCK    TFT_SCK
#define PIN_CS     TFT_CS
#define PIN_DC     TFT_DC
#define PIN_RST    TFT_RST

#define LCD_W 480
#define LCD_H 320

// Themes and Colors are now in ui_core.h

/* ── SPI handle ── */
static spi_device_handle_t s_spi = NULL;
char s_ip[32] = "0.0.0.0";
bool s_ip_dirty = true;
// Protects s_ip + s_ip_dirty against concurrent set_ip (event handler)
// vs. clock_task render. Without this a half-written IP string could
// be drawn on screen.
static portMUX_TYPE s_ip_mux = portMUX_INITIALIZER_UNLOCKED;
// Serializes access to the shared static buffers in fill_rect /
// set_window. Without it, two tasks calling fill_rect concurrently
// can corrupt the shared `last_color` / `buf` state — visible as
// pixel tearing or wrong-color rectangles in the UI.
static SemaphoreHandle_t s_disp_mutex = NULL;

/* ─────────────────────────────────────────────────────────────
   Low-level SPI
───────────────────────────────────────────────────────────── */
static inline void dc_cmd(void)  { gpio_set_level((gpio_num_t)PIN_DC, 0); }
static inline void dc_data(void) { gpio_set_level((gpio_num_t)PIN_DC, 1); }
static inline void cs_lo(void)   { gpio_set_level((gpio_num_t)PIN_CS, 0); }
static inline void cs_hi(void)   { gpio_set_level((gpio_num_t)PIN_CS, 1); }

static void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    spi_transaction_t t = {};
    cs_lo();
    dc_cmd();
    t.length = 8;
    t.tx_buffer = &cmd;
    spi_device_polling_transmit(s_spi, &t);

    if (data && len) {
        dc_data();
        t.length = len * 8;
        t.tx_buffer = data;
        spi_device_polling_transmit(s_spi, &t);
    }
    cs_hi();
}

/* ─────────────────────────────────────────────────────────────
   ST7796S init
───────────────────────────────────────────────────────────── */
static void st7796_init(void)
{
    gpio_set_level((gpio_num_t)PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd(0x01, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    { uint8_t d = 0x55; lcd_cmd(0x3A, &d, 1); }
    /* MADCTL: landscape rotated 180° relative to original orientation.
     *   0xE8 = MY|MX|MV|BGR (original)
     *   0x28 = MV|BGR       (180° rotation: drop MY and MX) */
    { uint8_t d = 0x28; lcd_cmd(0x36, &d, 1); }

    lcd_cmd(0x20, NULL, 0);
    lcd_cmd(0x13, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    lcd_cmd(0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ─────────────────────────────────────────────────────────────
   Drawing primitives
───────────────────────────────────────────────────────────── */
static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t ca[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    lcd_cmd(0x2A, ca, 4);
    lcd_cmd(0x2B, ra, 4);

    cs_lo();
    dc_cmd();
    uint8_t ramwr = 0x2C;
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &ramwr;
    spi_device_polling_transmit(s_spi, &t);
    dc_data();
}

void fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    if (x >= LCD_W || y >= LCD_H) return;
    if (x + w <= 0 || y + h <= 0) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;

    // Serialize fill_rect across all caller tasks. The static buf +
    // last_color cache below is shared state — concurrent tasks
    // would race the cache fill and corrupt each other's rects.
    if (s_disp_mutex) xSemaphoreTakeRecursive(s_disp_mutex, portMAX_DELAY);

    set_window(x, y, x + w - 1, y + h - 1);

    const int PIX_BATCH = 4096;
    static uint8_t buf[PIX_BATCH * 2];
    static uint16_t last_color = 0xFFFF;
    static bool init_buf = true;

    if (init_buf || color != last_color) {
        for (int i = 0; i < PIX_BATCH * 2; i += 2) {
            buf[i] = (uint8_t)(color >> 8);
            buf[i + 1] = (uint8_t)(color & 0xFF);
        }
        last_color = color;
        init_buf = false;
    }

    spi_transaction_t t = {};
    t.tx_buffer = buf;

    uint32_t pix = (uint32_t)w * h;
    while (pix > 0) {
        uint32_t batch = pix > PIX_BATCH ? PIX_BATCH : pix;
        t.length = batch * 16;
        spi_device_polling_transmit(s_spi, &t);
        pix -= batch;
    }
    cs_hi();
    if (s_disp_mutex) xSemaphoreGiveRecursive(s_disp_mutex);
}

extern "C" void ui_draw_rgb_bitmap(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* bitmap)
{
    if (x >= LCD_W || y >= LCD_H) return;
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;

    /* Serialise with fill_rect — both share the SPI device + the
     * set_window/CS/DC sequence. Without this mutex a fill_rect from
     * one task can overlap a ui_draw_rgb_bitmap from another (e.g.
     * dispense overlay vs clock task), interleaving CS/DC toggles and
     * corrupting the display. */
    if (s_disp_mutex) xSemaphoreTakeRecursive(s_disp_mutex, portMAX_DELAY);

    set_window(x, y, x + w - 1, y + h - 1);

    uint32_t pix = (uint32_t)w * h;
    const uint16_t* ptr = bitmap;

    // We send in chunks because the SPI DMA buffer has a max_transfer_sz limit.
    const uint32_t CHUNK_PIX = 16000;

    while (pix > 0) {
        uint32_t batch = pix > CHUNK_PIX ? CHUNK_PIX : pix;
        spi_transaction_t t = {};
        t.tx_buffer = ptr;
        t.length = batch * 16;
        spi_device_polling_transmit(s_spi, &t);
        ptr += batch;
        pix -= batch;
    }
    cs_hi();
    if (s_disp_mutex) xSemaphoreGiveRecursive(s_disp_mutex);
}

void draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    fill_rect(x, y, w, 2, color);
    fill_rect(x, y + h - 2, w, 2, color);
    fill_rect(x, y, 2, h, color);
    fill_rect(x + w - 2, y, 2, h, color);
}

void fill_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    fill_rect(x + r, y, w - 2 * r, h, color);

    for (int i = 0; i < r; i++) {
        int dy = r - i;
        int dx = r - (int)sqrtf((float)(r * r - dy * dy));
        int lw = r - dx;

        fill_rect(x + dx,     y + i,         lw, 1, color);
        fill_rect(x + w - r,  y + i,         lw, 1, color);
        fill_rect(x + dx,     y + h - 1 - i, lw, 1, color);
        fill_rect(x + w - r,  y + h - 1 - i, lw, 1, color);
    }

    fill_rect(x,       y + r, r, h - 2 * r, color);
    fill_rect(x + w-r, y + r, r, h - 2 * r, color);
}

void fill_round_rect_frame(int16_t x, int16_t y, int16_t w, int16_t h,
                                  int16_t r, uint16_t fill, uint16_t border)
{
    fill_round_rect(x, y, w, h, r, border);

    const int16_t border_thickness = 2;
    int16_t inner_x = x + border_thickness;
    int16_t inner_y = y + border_thickness;
    int16_t inner_w = w - (border_thickness * 2);
    int16_t inner_h = h - (border_thickness * 2);
    int16_t inner_r = r - border_thickness;

    if (inner_w > 0 && inner_h > 0) {
        if (inner_r < 0) inner_r = 0;
        fill_round_rect(inner_x, inner_y, inner_w, inner_h, inner_r, fill);
    }
}

void draw_gradient_v(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c1, uint16_t c2)
{
    int r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
    int r2 = (c2 >> 11) & 0x1F, g2 = (c2 >> 5) & 0x3F, b2 = c2 & 0x1F;

    for (int i = 0; i < h; i++) {
        uint8_t r = r1 + (r2 - r1) * i / h;
        uint8_t g = g1 + (g2 - g1) * i / h;
        uint8_t b = b1 + (b2 - b1) * i / h;
        uint16_t c = (r << 11) | (g << 5) | b;
        fill_rect(x, y + i, w, 1, c);
    }
}

void fill_screen(uint16_t color)
{
    fill_rect(0, 0, LCD_W, LCD_H, color);
}

/* ── Fonts included from ui_core.h ── */

int draw_char_gfx(int16_t x, int16_t y, unsigned char c, uint16_t fg, uint16_t bg, const GFXfont *font)
{
    if (!font) return 0;
    if (c < font->first || c > font->last) return 0;

    uint8_t cc = c - font->first;
    GFXglyph *glyph = &font->glyph[cc];
    const uint8_t *bitmap = font->bitmap;

    uint16_t bo = glyph->bitmapOffset;
    uint8_t gw = glyph->width, gh = glyph->height;
    int8_t xo = glyph->xOffset, yo = glyph->yOffset;
    uint8_t adv = glyph->xAdvance;

    int16_t top    = y + yo - 1;           // actual glyph top  - 1px padding
    int16_t bottom = y + yo + (int16_t)gh + 1; // actual glyph bot + 1px padding
    int16_t cell_h = bottom - top;
    if (cell_h < 1) cell_h = 1;

    static uint8_t row_buf[128];
    uint8_t bh = bg >> 8, bl = bg & 0xFF;
    uint8_t fh = fg >> 8, fl = fg & 0xFF;

    if (adv > 64) adv = 64;

    /* Serialise the whole char draw — without this, the static row_buf
     * gets clobbered by a concurrent draw (visible as garbled stripes
     * across the screen) and the set_window/CS/DC sequence below races
     * with fill_rect/ui_draw_rgb_bitmap from other tasks. Recursive
     * mutex — fill_rect inside cell-bg fill (if added) would re-enter. */
    if (s_disp_mutex) xSemaphoreTakeRecursive(s_disp_mutex, portMAX_DELAY);

    for (int16_t row = top; row < bottom; row++) {
        int16_t gy = row - (y + yo);

        for (uint8_t i = 0; i < adv; i++) {
            row_buf[i * 2] = bh;
            row_buf[i * 2 + 1] = bl;
        }

        if (gy >= 0 && gy < (int16_t)gh) {
            uint32_t bit_idx = (uint32_t)gy * gw;
            for (uint8_t gx = 0; gx < gw; gx++, bit_idx++) {
                uint8_t b = bitmap[bo + bit_idx / 8];
                bool on = (b >> (7 - (bit_idx & 7))) & 1;
                int16_t px = gx + xo;
                if (on && px >= 0 && px < adv) {
                    row_buf[px * 2] = fh;
                    row_buf[px * 2 + 1] = fl;
                }
            }
        }

        set_window(x, row, x + adv - 1, row);
        spi_transaction_t t = {};
        t.length = adv * 16;
        t.tx_buffer = row_buf;
        spi_device_polling_transmit(s_spi, &t);
        cs_hi();
    }
    if (s_disp_mutex) xSemaphoreGiveRecursive(s_disp_mutex);
    return adv;
}

int16_t draw_string_gfx(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg, const GFXfont *font)
{
    while (*str) {
        x += draw_char_gfx(x, y, *str++, fg, bg, font);
    }
    return x;
}

static int draw_char_gfx_scaled(int16_t x, int16_t y, unsigned char c, uint16_t fg, uint16_t bg, const GFXfont *font, uint8_t scale)
{
    if (!font) return 0;
    if (scale <= 1) return draw_char_gfx(x, y, c, fg, bg, font);
    if (c < font->first || c > font->last) return 0;

    uint8_t cc = c - font->first;
    GFXglyph *glyph = &font->glyph[cc];
    const uint8_t *bitmap = font->bitmap;

    uint16_t bo = glyph->bitmapOffset;
    uint8_t gw = glyph->width, gh = glyph->height;
    int8_t xo = glyph->xOffset, yo = glyph->yOffset;
    uint8_t adv = glyph->xAdvance;

    int16_t top = y + (yo * scale) - scale;
    int16_t cell_h = (gh * scale) + (2 * scale);
    int16_t cell_w = adv * scale;
    if (cell_w <= 0 || cell_h <= 0) return 0;

    fill_rect(x, top, cell_w, cell_h, bg);

    uint32_t bit_idx = 0;
    for (uint8_t gy = 0; gy < gh; gy++) {
        for (uint8_t gx = 0; gx < gw; gx++, bit_idx++) {
            uint8_t b = bitmap[bo + bit_idx / 8];
            bool on = (b >> (7 - (bit_idx & 7))) & 1;
            if (!on) continue;

            int16_t px = x + ((gx + xo) * scale);
            int16_t py = y + ((gy + yo) * scale);
            fill_rect(px, py, scale, scale, fg);
        }
    }

    return cell_w;
}

size_t safe_copy(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return 0;
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
    return strlen(dst);
}

int16_t gfx_text_width(const char *str, const GFXfont *font)
{
    if (!str || !font) return 0;
    int16_t w = 0;

    while (*str) {
        unsigned char c = (unsigned char)*str++;
        if (c < font->first || c > font->last) continue;
        const GFXglyph *glyph = &font->glyph[c - font->first];
        w += glyph->xAdvance;
    }
    return w;
}

int16_t draw_string_gfx_scaled(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg, const GFXfont *font, uint8_t scale)
{
    while (*str) {
        x += draw_char_gfx_scaled(x, y, *str++, fg, bg, font, scale);
    }
    return x;
}

int16_t gfx_text_width_scaled(const char *str, const GFXfont *font, uint8_t scale)
{
    if (scale <= 1) return gfx_text_width(str, font);
    return gfx_text_width(str, font) * scale;
}

void draw_string_centered(int16_t cx, int16_t baseline_y,
                                 const char *str, uint16_t fg, uint16_t bg,
                                 const GFXfont *font)
{
    int16_t w = gfx_text_width(str, font);
    int16_t x = cx - (w / 2);
    draw_string_gfx(x, baseline_y, str, fg, bg, font);
}

/* ─────────────────────────────────────────────────────────────
   SPI bus init
───────────────────────────────────────────────────────────── */
static void spi_display_bus_init(void)
{
    gpio_config_t io = {};
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_CS) | (1ULL << PIN_RST);
    gpio_config(&io);

    cs_hi();
    dc_data();

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = PIN_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = PIN_SCK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = LCD_W * 2 * 80;
    buscfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;

    esp_err_t r = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus fail");
        return;
    }

    spi_device_interface_config_t devcfg = {};
    /* Dropped 60 → 40 MHz (2026-05-14, second attempt). At 60 MHz the
     * user observed intermittent corruption — green smear in the dose
     * card, garbled OFFLINE/ONLINE pill bitmaps — exactly the failure
     * mode the original 60 MHz comment warned about. The Nano's TFT
     * goes over header pins + Dupont wires, signal integrity drops
     * sharply above ~50 MHz on that routing. 40 MHz is the safe-stable
     * value that worked for months before the bump. */
    devcfg.clock_speed_hz = 40 * 1000 * 1000;
    devcfg.mode = 0;
    devcfg.spics_io_num = -1;
    devcfg.queue_size = 1;
    devcfg.flags = SPI_DEVICE_HALFDUPLEX;

    spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);
    ESP_LOGI(TAG, "SPI2 OK 40MHz");
}

#include "ft6336u.h"

// Enum included from ui_core.h

enum ui_page_t current_page = PAGE_STANDBY;
enum ui_page_t pending_page = PAGE_STANDBY;
bool force_redraw = true;
static TickType_t s_intro_deadline = 0;

/* Warnings Modal States */
bool s_sched_warn_dismissed = false;
bool s_hw_warn_dismissed = false;
int s_popup_state = 0; // 0=None, 1=Sched, 2=HW, 3=Sync
uint32_t s_netpie_sync_popup_until = 0;

/* Time Picker States */
int edit_slot = -1;
int edit_hh = 0;
int edit_mm = 0;

wifi_ap_record_t scanned_aps[MAX_SCANNED_UI_APS];
uint16_t ap_count = 0;
char selected_ssid[33] = "";
bool is_wifi_setup = false;
int wifi_scroll = 0;
uint8_t wf_state = 0;
bool is_med_name_setup = false;
int selected_med_idx = 0;
int return_qty = 1;
bool show_return_confirm = false;



extern "C" void display_clock_set_ip(const char *ip)
{
    portENTER_CRITICAL(&s_ip_mux);
    safe_copy(s_ip, sizeof(s_ip), ip ? ip : "0.0.0.0");
    s_ip_dirty = true;
    portEXIT_CRITICAL(&s_ip_mux);
}

extern "C" void display_clock_init(void)
{
    ESP_LOGI(TAG, "Init ST7796S 480x320 - Interactive UI Layout");
    /* Recursive: drawing primitives nest (e.g. draw_char_gfx_scaled
     * calls fill_rect, which also takes this mutex). Non-recursive
     * would deadlock the moment any string draw runs. */
    if (!s_disp_mutex) s_disp_mutex = xSemaphoreCreateRecursiveMutex();
    spi_display_bus_init();
    if (!s_spi) {
        ESP_LOGE(TAG, "SPI fail");
        return;
    }
    ESP_LOGI(TAG, "Display SPI ready");
    st7796_init();
    ESP_LOGI(TAG, "Display controller initialized");
    // Avoid a full-screen clear in app_main(). The first UI task render will
    // paint the initial page and is less likely to trip instability during boot.
    force_redraw = true;
    ESP_LOGI(TAG, "Display initialized");
    // NOTE: settings_load_nvs() is called from main.c AFTER dfplayer_init()
    //       so that the Set Volume command reaches an initialized DFPlayer UART.
}

/* ─────────────────────────────────────────────────────────────
   Universal Top Bar Component
───────────────────────────────────────────────────────────── */
void draw_top_bar_with_back(const char *title)
{
    /* Top bar bumped from 44 to 56 px and back button nudged down so the
     * tap target sits ~16 px away from the panel edge (top edge is hard to
     * hit reliably on this TFT). Hit zone in callers also expanded to
     * include the top edge so misses near the bezel still register. */
    fill_rect(0, 0, LCD_W, 56, THEME_PANEL);
    fill_rect(0, 54, LCD_W, 2, THEME_ACCENT);

    // Back button (y 8→18, h 26→32)
    fill_rect(14, 18, 104, 32, THEME_ACCENT);
    draw_rect(14, 18, 104, 32, THEME_BORDER);

    // Arrow (shifted down by 16 px to match new button y)
    fill_rect(22, 35, 10, 2, THEME_TXT_MAIN);
    fill_rect(22, 35,  2, 2, THEME_TXT_MAIN);
    fill_rect(24, 33,  2, 2, THEME_TXT_MAIN);
    fill_rect(26, 31,  2, 2, THEME_TXT_MAIN);
    fill_rect(24, 37,  2, 2, THEME_TXT_MAIN);
    fill_rect(26, 39,  2, 2, THEME_TXT_MAIN);

    draw_string_gfx(40, 42, "BACK", THEME_TXT_MAIN, THEME_ACCENT, &FreeSans9pt7b);

    // Title (centered vertically in the taller bar)
    if (title) {
        draw_string_centered(LCD_W / 2, 39, title, THEME_TXT_MAIN, THEME_PANEL, &FreeSans18pt7b);
    }
}

/* ─────────────────────────────────────────────────────────────
   Universal Touch Coordinates Mapping
───────────────────────────────────────────────────────────── */
void ui_map_touch(uint16_t raw_x, uint16_t raw_y, uint16_t *ux, uint16_t *uy)
{
    /* Display flipped 180° in MADCTL; the touch panel is mounted in the
     * same orientation as the LCD, so the previous double-inversion is
     * no longer needed — pass raw coordinates straight through. */
    *ux = (raw_x < LCD_W) ? raw_x : (LCD_W - 1);
    *uy = (raw_y < LCD_H) ? raw_y : (LCD_H - 1);
}

static int count_selected_slots(uint8_t slots)
{
    int c = 0;
    for (int i = 0; i < 7; i++) {
        if (slots & (1 << i)) c++;
    }
    return c;
}



bool g_system_ready = false;

static void clock_task(void *)
{
    static uint32_t last_render_ticks = 0;
    static bool prev_touch = false;
    static uint32_t touch_start_ms = 0;
    static bool touch_handled = false;
    static uint16_t last_tx = 0, last_ty = 0;
    static uint32_t last_repeat_ms = 0;

    // Subscribe this task to the FreeRTOS task watchdog so if a touch I2C
    // read or display SPI op deadlocks for >TASK_WDT_TIMEOUT_S (45s) the
    // panic handler kicks in and reboots — far better than a frozen UI.
    esp_task_wdt_add(NULL);

    while (true) {
        if (!s_spi) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        esp_task_wdt_reset();

        /* Splash gate: hold the "เครื่องจ่ายยา / อัตโนมัติ" screen until
         * BOTH conditions are met:
         *   1. g_system_ready (deferred_init finished local resources)
         *   2. >= 2 s since boot (small floor so the splash isn't a
         *      single-frame flash on warm reboots).
         * Was 5 s, then 15 s historically — both were defensive after
         * touch flakiness that's since been root-caused (FT6336U RST
         * pulse + bus settle). User-visible boot wait is dominated by
         * g_system_ready latency now, so the floor just needs to be
         * long enough to look intentional, not as a safety margin. */
        {
            uint32_t uptime_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            bool splash_hold = (!g_system_ready) || (uptime_ms < 2000);
            if (splash_hold) {
                static bool boot_screen_drawn = false;
                if (!boot_screen_drawn) {
                    fill_screen(THEME_BG);
                    draw_utf8_centered_line_scaled(240, 148, "\xe0\xb9\x80\xe0\xb8\x84\xe0\xb8\xa3\xe0\xb8\xb7\xe0\xb9\x88\xe0\xb8\xad\xe0\xb8\x87\xe0\xb8\x88\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb8\xa2\xe0\xb8\xa2\xe0\xb8\xb2", 0xFFFF, THEME_BG, 40);
                    draw_utf8_centered_line_scaled(240, 198, "\xe0\xb8\xad\xe0\xb8\xb1\xe0\xb8\x95\xe0\xb9\x82\xe0\xb8\x99\xe0\xb8\xa1\xe0\xb8\xb1\xe0\xb8\x95\xe0\xb8\xb4", 0xAD55, THEME_BG, 36);
                    boot_screen_drawn = true;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                force_redraw = true;
                continue;
            }
        }

        uint16_t tx = 0, ty = 0;
        bool touched = false;
        /* Feed the TWDT before the I2C touch read — under bus contention
         * (mid-recovery cycle, contended I2C) ft6336u_read_touch can
         * stall up to 500 ms on the i2c_manager mutex. The top-of-loop
         * reset alone leaves a narrow TWDT margin if recovery + render
         * + touch stack unluckily; this extra reset widens the margin. */
        esp_task_wdt_reset();
        ft6336u_read_touch(&tx, &ty, &touched);

        if (touched) {
            last_tx = tx;
            last_ty = ty;
            /* Notify the idle-music driver that the user is active so
             * it stops playing track 99 (or doesn't start) for the
             * next 60 s. Symbol declared at file scope below. */
            idle_music_register_touch();
        }

        bool long_press = false;
        bool trigger_action = false;

        static uint32_t s_last_local_touch_ms = 0;

        if (touched && !prev_touch) {
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            touch_start_ms = now_ms;
            s_last_local_touch_ms = now_ms;
            last_repeat_ms = now_ms;
            long_press = false;
            
            static uint32_t s_last_trigger_ms = 0;
            if (now_ms - s_last_trigger_ms > 300) {
                trigger_action = true;
                s_last_trigger_ms = now_ms;
            } else {
                trigger_action = false;
            }
            touch_handled = true;
        } else if (touched && prev_touch) {
            if (!touch_handled && ((xTaskGetTickCount() * portTICK_PERIOD_MS) - touch_start_ms > 350)) {
                long_press = true;
                trigger_action = true;
                touch_handled = true;
            }
        } else if (!touched && prev_touch) {
            if (!touch_handled) {
                long_press = false;
                trigger_action = true;
                touch_handled = true;
            }
        }
        prev_touch = touched;

        if (trigger_action) {
            uint16_t atx = last_tx, aty = last_ty;

            uint16_t tx_n, ty_n;
            ui_map_touch(atx, aty, &tx_n, &ty_n);

            if (current_page == PAGE_STANDBY) {
                ui_standby_handle_touch(tx_n, ty_n);
            }
            else if (current_page == PAGE_MENU) {
                ui_menu_handle_touch(tx_n, ty_n);
            }
            else if (current_page == PAGE_KEYBOARD) {
                ui_keyboard_handle_touch(tx_n, ty_n);
            }
            else if (current_page == PAGE_WIFI_SCAN) {
                ui_wifi_scan_handle_touch(tx_n, ty_n);
            }
            else if (current_page == PAGE_WIFI_STATUS) {
                ui_wifi_status_handle_touch(tx_n, ty_n);
            }
            else if (current_page == PAGE_SETUP_SCHEDULE) {
                ui_setup_schedule_handle_touch(tx_n, ty_n);
            }
            else if (current_page == PAGE_TIME_PICKER) {
                ui_time_picker_handle_touch(tx_n, ty_n, long_press);
            }
            else if (current_page == PAGE_SETUP_MEDS) {
                ui_setup_meds_handle_touch(tx_n, ty_n);
            }
            else if (current_page == PAGE_SETUP_MEDS_DETAIL) {
                ui_setup_meds_detail_handle_touch(tx_n, ty_n);
            }
            else if (current_page == PAGE_MANUAL) {
                if (tx_n >= 0 && tx_n <= 130 && ty_n >= 0 && ty_n <= 52) pending_page = PAGE_MENU;
            }
            else if (current_page == PAGE_SETTINGS) {
                ui_settings_handle_touch(tx_n, ty_n);
            }
            else if (current_page == PAGE_CONFIRM_MEDS) {
                ui_confirm_handle_touch(tx_n, ty_n);
            }
        } // End of if (touched && !down)

        // --- Auto-switch to Confirm Page if medication is waiting ---
        static uint32_t s_last_play_ms = 0;

        if (dispenser_is_waiting()) {
            if (current_page != PAGE_CONFIRM_MEDS) {
                pending_page = PAGE_CONFIRM_MEDS;
            }

            // Loop the alarm sound every g_snd_alarm_interval_s seconds
            // (NVS-configurable from /tech > เสียง). Default 15 s lets a
            // long voice prompt finish playing before the next replay.
            if (current_page == PAGE_CONFIRM_MEDS || pending_page == PAGE_CONFIRM_MEDS) {
                extern int g_snd_alarm;
                extern int g_snd_alarm_interval_s;
                uint32_t period_ms = (uint32_t)g_snd_alarm_interval_s * 1000U;
                if (period_ms < 5000U) period_ms = 5000U;
                uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                if (s_last_play_ms == 0 || (now_ms - s_last_play_ms) >= period_ms) {
                    dfplayer_play_track(g_snd_alarm);
                    s_last_play_ms = now_ms;
                }
            }
        } else {
            s_last_play_ms = 0;
            if (current_page == PAGE_CONFIRM_MEDS) {
                pending_page = PAGE_STANDBY;
                dfplayer_stop();
            }
        }

        /* After a scheduled dispense, if a module came up empty or the IR
         * never saw a pill, jump straight into that module's detail page
         * so the user can refill or clear it. Set by execute_dispense
         * (dispenser task). Wait until the dispense is actually done and
         * we're back on standby — otherwise the popup-9 paint hides the
         * jump. */
        {
            extern volatile uint8_t g_dispense_missed_nav_mask;
            uint8_t snap = __atomic_load_n(&g_dispense_missed_nav_mask, __ATOMIC_SEQ_CST);
            if (snap != 0 &&
                !dispenser_is_busy() &&
                current_page == PAGE_STANDBY) {
                /* Pop lowest set bit → walks the user through every
                 * failed module in this slot, one Refill/Clear popup
                 * per module. Atomic AND so a concurrent set by the
                 * dispenser task (rare but possible during a fast
                 * slot run) can't drop our clear. */
                int idx = __builtin_ctz(snap);
                if (idx < DISPENSER_MED_COUNT) {
                    __atomic_fetch_and(&g_dispense_missed_nav_mask,
                                       (uint8_t)~(1u << idx), __ATOMIC_SEQ_CST);
                    selected_med_idx = idx;
                    pending_page = PAGE_SETUP_MEDS_DETAIL;
                    force_redraw = true;
                    ui_setup_meds_arm_refill_or_clear();
                    ESP_LOGI("DISP", "auto-nav to module %d detail (post-miss, snap=0x%02X)",
                             idx + 1, snap);
                } else {
                    /* Defensive — stale bit beyond med count, clear it. */
                    __atomic_store_n(&g_dispense_missed_nav_mask, 0, __ATOMIC_SEQ_CST);
                }
            }
        }

        if (current_page == PAGE_TIME_PICKER && touched) {
            uint16_t atx = last_tx, aty = last_ty;
            uint16_t tx_n, ty_n;
            ui_map_touch(atx, aty, &tx_n, &ty_n);

            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now_ms - touch_start_ms > 320) {
                if (now_ms - last_repeat_ms > 70) {
                    ui_time_picker_handle_hold(tx_n, ty_n);
                    last_repeat_ms = now_ms;
                }
            }
        }

        // Auto-return to standby after 5 min idle on any non-standby page.
        // Skips the med-alarm confirm popup (it owns its own dismiss flow)
        // and the boot intro. Touch activity resets the timer below.
        {
            static uint32_t s_last_activity_ms = 0;
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (trigger_action) {
                s_last_activity_ms = now_ms;
            }
            bool eligible = (current_page != PAGE_STANDBY &&
                             current_page != PAGE_INTRO &&
                             current_page != PAGE_CONFIRM_MEDS);
            if (!eligible) {
                s_last_activity_ms = now_ms;
            } else if (s_last_activity_ms != 0 &&
                       (now_ms - s_last_activity_ms) > (5U * 60U * 1000U)) {
                ESP_LOGI(TAG, "Idle timeout — returning to standby");
                pending_page = PAGE_STANDBY;
                s_last_activity_ms = now_ms;
            }
        }

        if (pending_page != current_page) {
            if (pending_page == PAGE_WIFI_SCAN) {
                wf_state = 0; // Reset scan state when entering the page!
            }
            /* Defensive cleanup of meds-detail edit session if the page
             * is being left some way other than Save/Back (e.g. a
             * scheduled-dispense Confirm popup yanks us away). Without
             * this the NETPIE publish inhibit pushed on entry would
             * leak and block all future shadow publishes until reboot.
             * PAGE_KEYBOARD is excluded — it's a transient sub-page
             * that returns to detail; the inhibit should stay up. */
            if (current_page == PAGE_SETUP_MEDS_DETAIL &&
                pending_page != PAGE_SETUP_MEDS_DETAIL &&
                pending_page != PAGE_KEYBOARD) {
                ui_setup_meds_end_edit_session_if_any();
            }
            current_page = pending_page;
            force_redraw = true;
        }

        uint32_t now = xTaskGetTickCount();
        bool page_needs_live_tick =
            (current_page == PAGE_STANDBY || current_page == PAGE_CONFIRM_MEDS ||
             current_page == PAGE_SETUP_MEDS || current_page == PAGE_SETUP_MEDS_DETAIL);
        bool page_needs_service_loop =
            (current_page == PAGE_TIME_PICKER || current_page == PAGE_WIFI_SCAN);
        // STANDBY needs 1 Hz to advance the seconds digit. Other live-tick
        // pages (CONFIRM_MEDS, SETUP_MEDS, SETUP_MEDS_DETAIL) have no
        // per-second content and were repainting their popups/status rows
        // 60 times a minute for nothing — that's where the screen-wide
        // flicker came from. Slow them to 4 Hz so the partial-update
        // diff still picks up changes quickly without doing whole-region
        // rewrites every second.
        uint32_t periodic_interval_ticks = pdMS_TO_TICKS(
            (current_page == PAGE_STANDBY) ? 1000 : 250);
        bool periodic_render =
            page_needs_live_tick && ((now - last_render_ticks) >= periodic_interval_ticks);

        static netpie_shadow_t s_last_sh_for_popup = {0};
        static bool s_popup_init = false;
        /* Atomic snapshot via netpie_shadow_copy — using netpie_get_shadow()
         * here and then memcpy'ing *curr_sh_ptr could read a half-published
         * buffer if the publish task flipped the double-buffer mid-copy
         * (the pointer is stable but the buffer it points to becomes the
         * "scratch" side after the flip). 500-byte struct memcpy is not
         * atomic. */
        netpie_shadow_t curr_sh_local = {0};
        bool have_shadow = netpie_shadow_copy(&curr_sh_local);
        const netpie_shadow_t *curr_sh_ptr = &curr_sh_local;

        if (!have_shadow) {
             /* No shadow yet (boot before NETPIE/NVS restored). Skip the
              * change-detection block; downstream code already handles
              * the un-init case via s_popup_init. */
        } else if (!s_popup_init && curr_sh_ptr->loaded) {
             s_last_sh_for_popup = *curr_sh_ptr;
             s_popup_init = true;
        } else if (s_popup_init && curr_sh_ptr->loaded) {
             bool shadow_changed = false;
             if (curr_sh_ptr->enabled != s_last_sh_for_popup.enabled) shadow_changed = true;
             else {
                 for (int i=0; i<7; i++) {
                     if (strcmp(curr_sh_ptr->slot_time[i], s_last_sh_for_popup.slot_time[i]) != 0) {
                         shadow_changed = true; break;
                     }
                 }
             }
             if (!shadow_changed) {
                 for (int i=0; i<DISPENSER_MED_COUNT; i++) {
                     if (curr_sh_ptr->med[i].count != s_last_sh_for_popup.med[i].count ||
                         curr_sh_ptr->med[i].slots != s_last_sh_for_popup.med[i].slots ||
                         strcmp(curr_sh_ptr->med[i].name, s_last_sh_for_popup.med[i].name) != 0) {
                         shadow_changed = true; break;
                     }
                 }
             }
             
             if (shadow_changed) {
                 uint32_t now_ms = now * portTICK_PERIOD_MS;
                 uint32_t local_touch_age = now_ms - s_last_local_touch_ms;
                 // Only force_redraw on structural shadow changes (schedule
                 // toggle, slot times, med name, slot mask). Pure count drift
                 // is handled by the per-page partial redraw (standby +
                 // setup-meds list) — a full fill_screen on every page would
                 // cause visible flicker.
                 bool structural_change = (curr_sh_ptr->enabled != s_last_sh_for_popup.enabled);
                 if (!structural_change) {
                     for (int i = 0; i < 7; i++) {
                         if (strcmp(curr_sh_ptr->slot_time[i], s_last_sh_for_popup.slot_time[i]) != 0) {
                             structural_change = true; break;
                         }
                     }
                 }
                 if (!structural_change) {
                     for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
                         if (curr_sh_ptr->med[i].slots != s_last_sh_for_popup.med[i].slots ||
                             strcmp(curr_sh_ptr->med[i].name, s_last_sh_for_popup.med[i].name) != 0) {
                             structural_change = true; break;
                         }
                     }
                 }
                 if (structural_change && local_touch_age > 2500) {
                     force_redraw = true;
                 }
                 s_netpie_sync_popup_until = 0;
                 s_last_sh_for_popup = *curr_sh_ptr;
             }
        }

        bool immediate_interaction_render = trigger_action;
        bool should_render =
            force_redraw || periodic_render || immediate_interaction_render || page_needs_service_loop;

        if (should_render) {
            if (force_redraw || periodic_render || page_needs_live_tick) {
                last_render_ticks = now;
            }

            char t_str[16] = "--:--:--";
            ds3231_get_time_str(t_str, sizeof(t_str));

            char hhmm[6] = "--:--";
            char ss[3] = "--";
            if (strlen(t_str) >= 8) {
                hhmm[0] = t_str[0];
                hhmm[1] = t_str[1];
                hhmm[2] = ':';
                hhmm[3] = t_str[3];
                hhmm[4] = t_str[4];
                hhmm[5] = '\0';

                ss[0] = t_str[6];
                ss[1] = t_str[7];
                ss[2] = '\0';
            }

            char date_str[20] = "";
            ds3231_get_date_str(date_str, sizeof(date_str));

            char dose_str[32] = "";
            dispenser_get_next_dose_str(dose_str, sizeof(dose_str));

            if (current_page == PAGE_INTRO) {
                if (force_redraw) {
                    const uint16_t BLUE_TOP = 0x0478;
                    draw_gradient_v(0, 0, LCD_W, LCD_H, BLUE_TOP, 0xFFFF);

                    fill_round_rect(180, 45, 120, 46, 23, 0xFFFF);
                    draw_string_gfx(200, 79, "MedBot", 0x0478, 0xFFFF, &FreeSans18pt7b);

                    draw_string_gfx(50, 155, "Welcome to your", 0xFFFF, BLUE_TOP, &FreeSans18pt7b);
                    draw_string_gfx(30, 200, "Medication Dispenser", 0xFFFF, BLUE_TOP, &FreeSans18pt7b);
                    draw_string_gfx(90, 245, "Smart Care  On Time", 0xB5B9, BLUE_TOP, &FreeSans12pt7b);

                    fill_round_rect(185, 282, 110, 26, 13, 0xFFFF);
                    draw_string_gfx(205, 302, "Starting...", 0x0478, 0xFFFF, &FreeSans9pt7b);
                    s_intro_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2500);
                }

                if (s_intro_deadline != 0 &&
                    (int32_t)(xTaskGetTickCount() - s_intro_deadline) >= 0) {
                    s_intro_deadline = 0;
                    pending_page = PAGE_STANDBY;
                    force_redraw = true;
                }
            }
            else if (current_page == PAGE_STANDBY) {
                ui_standby_render(now);
                /* Big-render pages can spend hundreds of ms in SPI font
                 * draws — feed the TWDT before falling through to the
                 * next loop iteration so we never accumulate close to
                 * the 45 s budget. */
                esp_task_wdt_reset();
            }
            else if (current_page == PAGE_MENU) {
                ui_menu_render();
            }
            else if (current_page == PAGE_SETUP_SCHEDULE) {
                ui_setup_schedule_render();
            }
            else if (current_page == PAGE_TIME_PICKER) {
                ui_time_picker_render();
            }
            else if (current_page == PAGE_SETUP_MEDS) {
                ui_setup_meds_render();
            }
            else if (current_page == PAGE_SETUP_MEDS_DETAIL) {
                ui_setup_meds_detail_render();
            }
            else if (current_page == PAGE_MANUAL) {
                if (force_redraw) {
                    fill_screen(THEME_BG);
                    draw_top_bar_with_back("Manual Dispense");
                    draw_string_gfx(120, 160, "Under Construction...", COLOR_MUTED, THEME_BG, &FreeSans12pt7b);
                }
            }
            else if (current_page == PAGE_SETTINGS) {
                ui_settings_render();
            }
            else if (current_page == PAGE_WIFI_SCAN) {
                ui_wifi_scan_render();
            }
            else if (current_page == PAGE_WIFI_STATUS) {
                ui_wifi_status_render();
            }
            else if (current_page == PAGE_KEYBOARD) {
                ui_keyboard_render();
            }
            else if (current_page == PAGE_CONFIRM_MEDS) {
                ui_confirm_render();
            }

            // Consume redraw request unless wifi_scan is transitioning from state 1 to 2
            if (!(current_page == PAGE_WIFI_SCAN && wf_state == 2)) {
                force_redraw = false; 
            }
        } // <-- This is the closing brace for the touched/redraw handling block

        if (usb_mouse_is_connected()) {
            static int16_t prev_mx = -1, prev_my = -1;
            static bool prev_lbtn = false;

            mouse_state_t ms = usb_mouse_get();

            auto get_cursor_bg = []() -> uint16_t {
                switch (current_page) {
                    case PAGE_MENU:        return THEME_PANEL;
                    case PAGE_KEYBOARD:    return KB_BG;
                    default:               return THEME_BG;
                }
            };

            if (ms.changed) {
                uint16_t bg = get_cursor_bg();

                if (prev_mx >= 0) {
                    fill_rect(prev_mx - 5, prev_my - 1, 12, 3, bg);
                    fill_rect(prev_mx - 1, prev_my - 5, 3, 12, bg);
                }

                fill_rect(ms.x - 5, ms.y, 10, 2, 0x000F);
                fill_rect(ms.x, ms.y - 5, 2, 10, 0x000F);

                prev_mx = ms.x;
                prev_my = ms.y;
            }

            if (ms.btn_left && !prev_lbtn) {
                last_tx = (uint16_t)(LCD_W - ms.x);
                last_ty = (uint16_t)(LCD_H - ms.y);
                touched = true;
                prev_touch = false;
                touch_handled = false;
                touch_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            }

            if (!ms.btn_left && prev_lbtn) {
                touched = false;
            }

            prev_lbtn = ms.btn_left;
        }

        // Lower the loop rate from ~60-100Hz to ~20-30Hz so the FT6336U
        // touch read (i2c_manager_read_reg) doesn't pummel the I2C bus
        // and trigger the ESP-IDF v5.3 i2c_master ISR race more often.
        TickType_t ui_sleep = pdMS_TO_TICKS(50);
        if (current_page == PAGE_KEYBOARD || current_page == PAGE_TIME_PICKER) {
            ui_sleep = pdMS_TO_TICKS(33);
        }
        if (touched) {
            ui_sleep = pdMS_TO_TICKS(25);
        }
        vTaskDelay(ui_sleep);
    }
}

extern "C" void display_clock_start_task(void)
{
    if (xTaskCreate(clock_task, "clk_task", 10240, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create clock task");
        return;
    }
    ESP_LOGI(TAG, "Clock task started");
}

// display_clock_show_ultra_safe removed 2026-05-10 with safe_mode logic.
