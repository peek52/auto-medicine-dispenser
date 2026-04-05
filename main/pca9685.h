#pragma once
#include "esp_err.h"
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

/** Set raw PWM on/off counts for a channel (0–4095) */
esp_err_t pca9685_set_pwm(uint8_t channel, uint16_t on, uint16_t off);

/** Set servo angle (0–180 degrees) */
esp_err_t pca9685_set_angle(uint8_t channel, int angle);

/** Move channel to home_angle */
esp_err_t pca9685_go_home(uint8_t channel);

/** Move channel to work_angle */
esp_err_t pca9685_go_work(uint8_t channel);

/** Save home/work for a channel (in RAM only, no NVS) */
void pca9685_set_positions(uint8_t channel, int home, int work);

#ifdef __cplusplus
}
#endif
