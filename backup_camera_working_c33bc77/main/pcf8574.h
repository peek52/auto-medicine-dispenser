#pragma once
#include "esp_err.h"
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

#ifdef __cplusplus
}
#endif
