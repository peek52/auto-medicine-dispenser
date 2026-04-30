#include "dfplayer.h"
#include "config.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DY_HV20T";
static const bool s_audio_enabled = true;
static bool s_uart_ready = false;

// Configuration validation
#ifndef DFPLAYER_UART_NUM
#define DFPLAYER_UART_NUM   UART_NUM_1
#define DFPLAYER_TX_PIN     22
#define DFPLAYER_RX_PIN     23
#endif

static void dy_send_cmd(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    if (!s_audio_enabled) {
        return;
    }
    if (!s_uart_ready) {
        ESP_LOGW(TAG, "Audio UART not ready, skipping cmd 0x%02X", cmd);
        return;
    }

    uint8_t buf[16];
    uint8_t sum = 0;
    int idx = 0;

    buf[idx++] = 0xAA;
    sum += 0xAA;

    buf[idx++] = cmd;
    sum += cmd;

    buf[idx++] = len;
    sum += len;

    for (uint8_t i = 0; i < len; i++) {
        buf[idx++] = data[i];
        sum += data[i];
    }

    buf[idx++] = sum;
    int written = uart_write_bytes(DFPLAYER_UART_NUM, (const char *)buf, idx);
    if (written < 0) {
        ESP_LOGW(TAG, "UART write failed for cmd 0x%02X, disabling audio UART until reinit", cmd);
        s_uart_ready = false;
    }
}

void dfplayer_init(void)
{
    s_uart_ready = false;

    if (!s_audio_enabled) {
        ESP_LOGW(TAG, "Audio disabled in firmware; skipping audio module init");
        return;
    }

    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (!uart_is_driver_installed(DFPLAYER_UART_NUM)) {
        esp_err_t ret = uart_driver_install(DFPLAYER_UART_NUM, 256, 0, 0, NULL, 0);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to install audio UART driver: %s", esp_err_to_name(ret));
            return;
        }
    }

    esp_err_t ret = uart_param_config(DFPLAYER_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure audio UART: %s", esp_err_to_name(ret));
        return;
    }

    ret = uart_set_pin(DFPLAYER_UART_NUM, DFPLAYER_TX_PIN, DFPLAYER_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to assign audio UART pins: %s", esp_err_to_name(ret));
        return;
    }

    s_uart_ready = true;

    ESP_LOGI(TAG, "DY-HV20T UART initialized on TX:%d RX:%d", DFPLAYER_TX_PIN, DFPLAYER_RX_PIN);

    // Give the module a moment to boot before sending commands.
    vTaskDelay(pdMS_TO_TICKS(500));

    dfplayer_set_volume(15);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static uint8_t s_current_hw_vol = 255;
static bool s_is_eng_mode = false;

void dfplayer_set_language(int lang_is_eng)
{
    s_is_eng_mode = (lang_is_eng != 0);
    ESP_LOGI(TAG, "Audio language set to: %s", s_is_eng_mode ? "English" : "Thai");
}

void dfplayer_set_volume(uint8_t vol)
{
    if (vol > 30) vol = 30;
    if (vol == s_current_hw_vol) return;

    if (!s_audio_enabled || !s_uart_ready) {
        s_current_hw_vol = vol;
        return;
    }

    uint8_t data[1] = { vol };
    dy_send_cmd(0x13, data, 1);
    s_current_hw_vol = vol;
    ESP_LOGI(TAG, "Set Volume: %d", vol);
}

void dfplayer_play_track(uint16_t num)
{
    // Volume classification: only the actual alarm/pre-alert clips
    // (tracks 1-4) should play at the louder alert volume. Nav-toggle
    // feedback (34-36) and system/language/menu feedback (80-99) used
    // to fall through this gate too, which made tap-feedback sounds
    // (e.g. ตารางยาวันนี้ popup taps 89-94) noticeably louder than
    // ordinary button taps. Treat those as nav-volume sounds instead.
    bool is_alert = (num >= 1 && num <= 4);
    // "must always play" — bypass the nav-sound-enabled toggle so
    // critical feedback (alarms + nav toggle + system clips) still
    // plays even when the user has nav sounds disabled.
    bool always_play = is_alert ||
                       (num == 34) || (num == 35) || (num == 36) ||
                       (num >= 80 && num <= 99);

    extern int g_alert_volume;
    extern int g_nav_volume;
    extern bool g_nav_sound_enabled;

    if (!s_audio_enabled || !s_uart_ready) {
        return;
    }

    if (!always_play && !g_nav_sound_enabled) {
        ESP_LOGI(TAG, "Navigation sound disabled, skipping track %d", num);
        return;
    }

    uint8_t target_vol = is_alert ? g_alert_volume : g_nav_volume;
    if (s_current_hw_vol != target_vol) {
        dfplayer_set_volume(target_vol);
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    uint16_t actual_num = (s_is_eng_mode && num < 80) ? (num + 40) : num;

    uint8_t data[2] = {
        (uint8_t)((actual_num >> 8) & 0xFF),
        (uint8_t)(actual_num & 0xFF)
    };
    dy_send_cmd(0x07, data, 2);
    ESP_LOGI(TAG, "Playing Track: %d (HW Vol: %d)", actual_num, target_vol);
}

void dfplayer_play_track_force_vol(uint16_t num, uint8_t force_vol)
{
    if (!s_audio_enabled || !s_uart_ready) {
        return;
    }

    if (s_current_hw_vol != force_vol) {
        dfplayer_set_volume(force_vol);
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    uint16_t actual_num = (s_is_eng_mode && num < 80) ? (num + 40) : num;

    uint8_t data[2] = {
        (uint8_t)((actual_num >> 8) & 0xFF),
        (uint8_t)(actual_num & 0xFF)
    };
    dy_send_cmd(0x07, data, 2);
    ESP_LOGI(TAG, "Playing Track: %d (HW Vol: %d [FORCED])", actual_num, force_vol);
}

void dfplayer_stop(void)
{
    if (!s_audio_enabled || !s_uart_ready) {
        return;
    }

    dy_send_cmd(0x04, NULL, 0);
    ESP_LOGI(TAG, "Stopped Playback");
}
