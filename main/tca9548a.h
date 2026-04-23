#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  tca9548a.h — TCA9548A I2C Multiplexer driver
//  ใช้เป็น channel selector สำหรับ VL53L0X 6 ตัว (channel 0-5)
//  Address: 0x70 (A0=A1=A2=GND)
// ─────────────────────────────────────────────────────────────────────────────

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize TCA9548A — disable all channels at boot
esp_err_t tca9548a_init(void);

// Select a single channel (0-7), disable all others
// Returns ESP_OK on success
esp_err_t tca9548a_select_channel(uint8_t ch);

// Disable all channels (write 0x00)
esp_err_t tca9548a_disable_all(void);

// Ping TCA9548A to check if it's present on I2C bus
bool tca9548a_is_present(void);

#ifdef __cplusplus
}
#endif
