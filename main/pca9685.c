#include "pca9685.h"
#include "i2c_manager.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pca9685";

/* Servo-busy flag. Volatile bool so the touch poll task sees writes
 * from the dispense path without locking. */
static volatile bool s_servo_busy = false;
void pca9685_servo_busy_set(bool busy) { s_servo_busy = busy; }
bool pca9685_servo_busy_get(void) { return s_servo_busy; }

/* Per-channel ramp lock. Stops a second async ramp from spawning before
 * the previous one on the same channel has finished — otherwise two
 * pca_ramp tasks race against each other writing PWM to the same
 * channel, producing jitter and confusing the dispenser's
 * "cur_angle == target" completion poll. */
static SemaphoreHandle_t s_ramp_lock[PCA9685_NUM_CHANNELS] = {0};
static SemaphoreHandle_t ramp_lock_get(uint8_t channel) {
    if (channel >= PCA9685_NUM_CHANNELS) return NULL;
    if (!s_ramp_lock[channel]) {
        s_ramp_lock[channel] = xSemaphoreCreateMutex();
    }
    return s_ramp_lock[channel];
}

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

void pca9685_load_cache_only(void)
{
    for (int i = 0; i < PCA9685_NUM_CHANNELS; i++) {
        g_servo[i].home_angle = 66;
        g_servo[i].work_angle = 33;
        g_servo[i].cur_angle  = -1;
    }
    pca9685_load_nvs();
}

esp_err_t pca9685_init(void)
{
    // Populate the runtime cache up front — even if the hardware
    // probe below fails, /servo/state should still report the user's
    // saved home/work angles instead of BSS-zero 0/0.
    pca9685_load_cache_only();

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

    // (Defaults + NVS load now happen at the top of pca9685_init so
    //  the web /servo/state shows the right values even when the chip
    //  isn't physically present.)

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
    // Retry on transient bus glitches (servo current spikes can corrupt
    // a single I2C transaction). 5 attempts with 10 ms gap is enough to
    // ride out brown-out wobble without bricking the dispense flow.
    esp_err_t r = ESP_FAIL;
    for (int attempt = 0; attempt < 5; ++attempt) {
        r = i2c_manager_write(ADDR_PCA9685, buf, 5);
        if (r == ESP_OK) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "PCA9685 set_pwm ch=%d failed after retries: %s",
             channel, esp_err_to_name(r));
    return r;
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

    /* Write FIRST — only commit cur_angle if the PWM update actually
     * landed. Otherwise the dispenser's "cur_angle == target" completion
     * poll would return success on a failed I2C burst, sending the
     * scheduler past a partial ramp before the servo physically moved. */
    esp_err_t r = pca9685_set_pwm(channel, 0, off);
    if (r == ESP_OK) {
        g_servo[channel].cur_angle = angle;
    }
    return r;
}

// Ramped move: step the servo gradually toward target instead of
// snapping. Reduces servo current inrush (less rail dip / EMI on the
// shared 5 V) and softens mechanical jolt that was causing IR sensor
// false-trigger and display SPI glitch during dispense.
//
// Tuning: 1°/step × 30 ms = ~2700 ms for a 90° throw.
// History:
//   5°/20 ms → 2°/30 ms (2026-05-11, "softer")
//   2°/30 ms → 1°/30 ms (2026-05-11, "ช้าปานกลาง")
//   1°/30 ms → 2°/40 ms (2026-05-11, "smoother w/o EMI buildup")
//   2°/40 ms → 2°/60 ms (2026-05-13, gentler on 5 V rail)
//   2°/60 ms → 1°/20 ms (2026-05-13, IR removed, fast + smooth)
//   1°/20 ms → 1°/30 ms (2026-05-13, "เร็วไป + servo อื่นเพี้ยน" —
//     20 ms updates were two-per-PWM-frame in some cases, racing the
//     internal latch and inducing micro-glitches on neighbouring
//     channels. 30 ms = one update per 50 Hz PWM frame, also gives
//     more time for the 5 V rail to recover between steps.)
#define PCA9685_RAMP_STEP_DEG   1
#define PCA9685_RAMP_STEP_MS    30

static esp_err_t pca9685_set_angle_ramped(uint8_t channel, int target_angle)
{
    if (channel >= PCA9685_NUM_CHANNELS) return ESP_ERR_INVALID_ARG;
    if (target_angle < 0)   target_angle = 0;
    if (target_angle > 180) target_angle = 180;

    int cur = g_servo[channel].cur_angle;

    /* First-ever command after boot: cur_angle == -1 (no PWM has been
     * established yet). If we ramp from -1, the first PWM command is a
     * near-0° pulse which makes the servo snap from its physical
     * resting position (~home) to ~0° at full speed.
     *
     * Cheap fix: pretend we're already at home and ramp from there.
     * The servo can't physically catch up to a 2°/30ms ramp anyway, so
     * it'll smoothly track from its actual position toward target —
     * no need to seed PWM or sleep (those caused other tasks to stall
     * on the shared I2C bus). */
    if (cur < 0) {
        int home = g_servo[channel].home_angle;
        if (home < 0)   home = 0;
        if (home > 180) home = 180;
        g_servo[channel].cur_angle = home;
        cur = home;
    }

    if (cur == target_angle) {
        // Already there — still write once so PWM is refreshed.
        return pca9685_set_angle(channel, target_angle);
    }

    int dir  = (target_angle > cur) ? 1 : -1;
    int step = PCA9685_RAMP_STEP_DEG;
    if (step < 1) step = 1;

    // Signal "servo busy" for the duration of the ramp so the touch
    // driver can skip its I2C poll during the PWM-noise window. (Touch
    // IC shares the bus and sees coupled noise during ramps.)
    pca9685_servo_busy_set(true);

    while (cur != target_angle) {
        int next = cur + dir * step;
        if ((dir > 0 && next > target_angle) ||
            (dir < 0 && next < target_angle)) {
            next = target_angle;
        }
        esp_err_t err = pca9685_set_angle(channel, next);
        if (err != ESP_OK) {
            pca9685_servo_busy_set(false);
            return err;
        }
        cur = next;
        if (cur != target_angle) {
            vTaskDelay(pdMS_TO_TICKS(PCA9685_RAMP_STEP_MS));
        }
    }
    // Small settle for the last PWM transient to die out before we let
    // the touch driver poll again.
    vTaskDelay(pdMS_TO_TICKS(40));
    pca9685_servo_busy_set(false);
    return ESP_OK;
}

esp_err_t pca9685_go_home(uint8_t channel)
{
    return pca9685_set_angle_ramped(channel, g_servo[channel].home_angle);
}

esp_err_t pca9685_go_work(uint8_t channel)
{
    return pca9685_set_angle_ramped(channel, g_servo[channel].work_angle);
}

/* Async ramp worker. Drives one ramp then self-deletes. */
typedef struct {
    uint8_t channel;
    int     target_angle;
} pca_async_arg_t;

static void pca9685_ramp_task(void *arg)
{
    pca_async_arg_t *a = (pca_async_arg_t *)arg;
    SemaphoreHandle_t lock = ramp_lock_get(a->channel);
    /* Wait for any in-flight ramp on this channel to finish — without
     * this, sequential go_work_async + go_home_async would spawn two
     * ramp tasks racing to write PWM on the same channel. 10 s
     * deadline is the worst-case for a 180° ramp at 1°/30 ms. */
    if (lock) xSemaphoreTake(lock, pdMS_TO_TICKS(10000));
    (void)pca9685_set_angle_ramped(a->channel, a->target_angle);
    if (lock) xSemaphoreGive(lock);
    free(a);
    vTaskDelete(NULL);
}

static esp_err_t pca9685_go_angle_async(uint8_t channel, int target)
{
    if (channel >= PCA9685_NUM_CHANNELS) return ESP_ERR_INVALID_ARG;
    pca_async_arg_t *a = (pca_async_arg_t *)malloc(sizeof(*a));
    if (!a) return ESP_ERR_NO_MEM;
    a->channel = channel;
    a->target_angle = target;
    /* Priority 5 = above idle but below the dispense scheduler (typically
     * 6+). Slightly higher than touch (4) so the ramp finishes promptly. */
    BaseType_t ok = xTaskCreate(pca9685_ramp_task, "pca_ramp", 3072, a, 5, NULL);
    if (ok != pdPASS) {
        free(a);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t pca9685_go_home_async(uint8_t channel)
{
    if (channel >= PCA9685_NUM_CHANNELS) return ESP_ERR_INVALID_ARG;
    return pca9685_go_angle_async(channel, g_servo[channel].home_angle);
}

esp_err_t pca9685_go_work_async(uint8_t channel)
{
    if (channel >= PCA9685_NUM_CHANNELS) return ESP_ERR_INVALID_ARG;
    return pca9685_go_angle_async(channel, g_servo[channel].work_angle);
}

void pca9685_set_positions(uint8_t channel, int home, int work)
{
    if (channel >= PCA9685_NUM_CHANNELS) return;
    g_servo[channel].home_angle = home;
    g_servo[channel].work_angle = work;
}
