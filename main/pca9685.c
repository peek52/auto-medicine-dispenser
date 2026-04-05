#include "pca9685.h"
#include "i2c_manager.h"
#include "config.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pca9685";

// PCA9685 registers
#define PCA9685_MODE1       0x00
#define PCA9685_PRESCALE    0xFE
#define PCA9685_LED0_ON_L   0x06  // Channel 0 base; each channel = +4 bytes
#define PCA9685_OSC_CLOCK   25000000  // 25 MHz internal oscillator

pca9685_servo_cfg_t g_servo[PCA9685_NUM_CHANNELS];

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_manager_write(ADDR_PCA9685, buf, 2);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_manager_read_reg(ADDR_PCA9685, reg, val, 1);
}

/* ── NVS Load/Save for Servo Positions ── */
void pca9685_load_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open("servo_cfg", NVS_READONLY, &handle) == ESP_OK) {
        for (int i = 0; i < PCA9685_NUM_CHANNELS; i++) {
            char key_h[16], key_w[16];
            snprintf(key_h, sizeof(key_h), "ch%d_h", i);
            snprintf(key_w, sizeof(key_w), "ch%d_w", i);

            int16_t h = g_servo[i].home_angle;
            int16_t w = g_servo[i].work_angle;

            nvs_get_i16(handle, key_h, &h);
            nvs_get_i16(handle, key_w, &w);

            // Upgrade legacy NVS presets instantly preventing old defaults from breaking execution
            if (h == 10 && w == 90) {
                h = 66;
                w = 33;
            }
            if (h == 0 && w == 0) {
                h = 66;
                w = 33;
            }
            if (h == 75 && w == 20) {
                h = 66;
                w = 33;
            }
            if (h == 65 && w == 33) {
                h = 66;
                w = 33;
            }

            g_servo[i].home_angle = h;
            g_servo[i].work_angle = w;
        }
        nvs_close(handle);
        ESP_LOGI(TAG, "Servo configuration loaded from NVS");
    } else {
        ESP_LOGI(TAG, "No NVS config for servo found, using defaults");
    }
}

void pca9685_save_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open("servo_cfg", NVS_READWRITE, &handle) == ESP_OK) {
        for (int i = 0; i < PCA9685_NUM_CHANNELS; i++) {
            char key_h[16], key_w[16];
            snprintf(key_h, sizeof(key_h), "ch%d_h", i);
            snprintf(key_w, sizeof(key_w), "ch%d_w", i);

            nvs_set_i16(handle, key_h, (int16_t)g_servo[i].home_angle);
            nvs_set_i16(handle, key_w, (int16_t)g_servo[i].work_angle);
        }
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Servo configuration saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for saving servo config");
    }
}

esp_err_t pca9685_init(void)
{
    // Reset
    esp_err_t ret = write_reg(PCA9685_MODE1, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCA9685 not found at 0x40: %s", esp_err_to_name(ret));
        return ret;
    }

    // Sleep mode to set prescaler
    ret = write_reg(PCA9685_MODE1, 0x10);
    if (ret != ESP_OK) return ret;

    // Prescaler for 50 Hz: prescale = round(osc / (4096 * freq)) - 1
    uint8_t prescale = (uint8_t)roundf((float)PCA9685_OSC_CLOCK / (4096.0f * SERVO_FREQ_HZ) - 1.0f);
    ret = write_reg(PCA9685_PRESCALE, prescale);
    if (ret != ESP_OK) return ret;

    // Wake up
    ret = write_reg(PCA9685_MODE1, 0x00);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(5));

    // Enable auto-increment
    ret = write_reg(PCA9685_MODE1, 0xA0);
    if (ret != ESP_OK) return ret;

    // Init servo configs to defaults first
    for (int i = 0; i < PCA9685_NUM_CHANNELS; i++) {
        g_servo[i].home_angle = 66;
        g_servo[i].work_angle = 33;
        g_servo[i].cur_angle  = -1;
    }

    // Load actual configurations from NVS overriding defaults if exist
    pca9685_load_nvs();

    ESP_LOGI(TAG, "PCA9685 initialized (prescale=%d, ~50Hz)", prescale);
    return ESP_OK;
}

esp_err_t pca9685_set_pwm(uint8_t channel, uint16_t on, uint16_t off)
{
    if (channel >= PCA9685_NUM_CHANNELS) return ESP_ERR_INVALID_ARG;
    uint8_t base = PCA9685_LED0_ON_L + 4 * channel;
    uint8_t buf[5] = {
        base,
        (uint8_t)(on & 0xFF),
        (uint8_t)(on >> 8),
        (uint8_t)(off & 0xFF),
        (uint8_t)(off >> 8),
    };
    return i2c_manager_write(ADDR_PCA9685, buf, 5);
}

esp_err_t pca9685_set_angle(uint8_t channel, int angle)
{
    if (angle < 0)   angle = 0;
    if (angle > 180) angle = 180;

    // Map angle → pulse width in microseconds
    int pulse_us = SERVO_MIN_PULSEWIDTH_US +
                   (angle * (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US)) / 180;

    // pulse_us → off count (period = 20000 us = 4096 counts)
    uint16_t off = (uint16_t)((pulse_us * 4096) / 20000);

    g_servo[channel].cur_angle = angle;
    return pca9685_set_pwm(channel, 0, off);
}

esp_err_t pca9685_go_home(uint8_t channel)
{
    return pca9685_set_angle(channel, g_servo[channel].home_angle);
}

esp_err_t pca9685_go_work(uint8_t channel)
{
    return pca9685_set_angle(channel, g_servo[channel].work_angle);
}

void pca9685_set_positions(uint8_t channel, int home, int work)
{
    if (channel >= PCA9685_NUM_CHANNELS) return;
    g_servo[channel].home_angle = home;
    g_servo[channel].work_angle = work;
}
