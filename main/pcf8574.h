#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * pcf8574 — IR sensor interface via PCF8574 GPIO expander.
 * Address: ADDR_PCF8574 (0x20)
 */

/** Set all PCF8574 pins to input mode (write 0xFF) */
esp_err_t pcf8574_set_all_input(void);

/**
 * Read all 8 pins from PCF8574.
 * bit[i] = 0 → DETECTED (active low), 1 → CLEAR
 */
esp_err_t pcf8574_read(uint8_t *val_out);

/**
 * Block PCF8574 reads during servo ramp. When `blocked` is true,
 * pcf8574_read() returns ESP_OK with val_out=0xFF (no detection)
 * instead of going to the I2C bus. Servo PWM coupling onto the IR
 * module OUT line during ramp transitions produces phantom LOW
 * spikes; blocking reads keeps the dispense loop's debounce counter
 * from latching onto noise. Caller must clear the flag after the
 * ramp completes (and ideally after a small settle delay).
 */
void pcf8574_block_during_servo_ramp(bool blocked);

/**
 * True while a servo ramp is in progress. Other I2C clients (e.g. the
 * FT6336U touch driver) can poll this and skip their consecutive-fail
 * counters during the window — transient bus glitches caused by servo
 * PWM coupling are expected here and shouldn't trigger heavy recovery
 * (e.g. CTP_RST + bus tear-down) that leaves the chip in fallback mode.
 */
bool pcf8574_is_servo_ramping(void);

#ifdef __cplusplus
}
#endif
