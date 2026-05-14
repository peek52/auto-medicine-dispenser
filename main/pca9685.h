#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * pca9685 — PWM servo driver via PCA9685.
 * Address: ADDR_PCA9685 (0x40)
 * Generates 50 Hz PWM on 16 channels.
 */

#define PCA9685_NUM_CHANNELS  16

typedef struct {
    int home_angle;   // degrees
    int work_angle;   // degrees
    int cur_angle;    // current position
} pca9685_servo_cfg_t;

extern pca9685_servo_cfg_t g_servo[PCA9685_NUM_CHANNELS];

/** Initialise PCA9685: set oscillator mode and configure 50 Hz PWM */
esp_err_t pca9685_init(void);

/**
 * Initialise g_servo[] cache (defaults + NVS) without touching the
 * hardware. Call this from boot when the PCA9685 chip is not present
 * so the web UI still sees sensible home/work positions.
 */
void pca9685_load_cache_only(void);

/** Set raw PWM on/off counts for a channel (0–4095) */
esp_err_t pca9685_set_pwm(uint8_t channel, uint16_t on, uint16_t off);

/** Set servo angle (0–180 degrees) */
esp_err_t pca9685_set_angle(uint8_t channel, int angle);

/** Move channel to home_angle */
esp_err_t pca9685_go_home(uint8_t channel);

/** Move channel to work_angle */
esp_err_t pca9685_go_work(uint8_t channel);

/**
 * Non-blocking variants — spawn a one-shot task to drive the ramp.
 * Returns immediately (ESP_OK once the task is queued). Caller is
 * responsible for waiting any settle time before issuing the next
 * command. Used by the dispense IR poll loop so the 500 Hz IR sampling
 * is not frozen during the ~2.7 s ramp.
 */
esp_err_t pca9685_go_home_async(uint8_t channel);
esp_err_t pca9685_go_work_async(uint8_t channel);

/** Save home/work for a channel (in RAM only, no NVS) */
void pca9685_set_positions(uint8_t channel, int home, int work);

/** Servo-ramp busy flag. Set TRUE while a PWM ramp is driving the bus
 * (PCA9685 sends a burst of I2C writes per degree). The ft6336u touch
 * driver checks this and skips a poll cycle, otherwise the touch IC
 * sees PWM-coupled noise on its own I2C lines and returns garbage. */
void pca9685_servo_busy_set(bool busy);
bool pca9685_servo_busy_get(void);

#ifdef __cplusplus
}
#endif
