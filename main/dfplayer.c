#include "dfplayer.h"
#include "config.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DY_T20L";
static const bool s_audio_enabled = false;

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
    uart_write_bytes(DFPLAYER_UART_NUM, (const char *)buf, idx);
}

void dfplayer_init(void)
{
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

    ESP_ERROR_CHECK(uart_driver_install(DFPLAYER_UART_NUM, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(DFPLAYER_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(DFPLAYER_UART_NUM, DFPLAYER_TX_PIN, DFPLAYER_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "DY-T20L UART initialized on TX:%d RX:%d", DFPLAYER_TX_PIN, DFPLAYER_RX_PIN);

    // Give the module a moment to boot before sending commands.
    vTaskDelay(pdMS_TO_TICKS(500));

    dfplayer_set_volume(15);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static uint8_t s_current_hw_vol = 255;

void dfplayer_set_volume(uint8_t vol)
{
    if (vol > 30) vol = 30;
    if (vol == s_current_hw_vol) return;

    if (!s_audio_enabled) {
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
    // Tracks 1-4 = Alarm/Pre-alerts, track 34 = boot intro, 35-36 = nav toggle feedback (must always play)
    bool is_alert = (num >= 1 && num <= 4) || (num == 34) || (num == 35) || (num == 36);
    
    extern int g_alert_volume;
    extern int g_nav_volume;
    extern bool g_nav_sound_enabled;

    if (!s_audio_enabled) {
        return;
    }

    if (!is_alert && !g_nav_sound_enabled) {
        ESP_LOGI(TAG, "Navigation sound disabled, skipping track %d", num);
        return;
    }

    uint8_t target_vol = is_alert ? g_alert_volume : g_nav_volume;
    if (s_current_hw_vol != target_vol) {
        dfplayer_set_volume(target_vol);
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    uint8_t data[2] = {
        (uint8_t)((num >> 8) & 0xFF),
        (uint8_t)(num & 0xFF)
    };
    dy_send_cmd(0x07, data, 2);
    ESP_LOGI(TAG, "Playing Track: %d (HW Vol: %d)", num, target_vol);
}

void dfplayer_stop(void)
{
    if (!s_audio_enabled) {
        return;
    }

    dy_send_cmd(0x04, NULL, 0);
    ESP_LOGI(TAG, "Stopped Playback");
}
