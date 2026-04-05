#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the FT6336U touch controller over I2C
 * @return ESP_OK on success
 */
esp_err_t ft6336u_init(void);

/**
 * @brief Read the current touch state
 * @param[out] x Pointer to store X coordinate
 * @param[out] y Pointer to store Y coordinate
 * @param[out] pressed Pointer to store touch state (true if pressed)
 * @return ESP_OK on successful read (even if not touched)
 */
esp_err_t ft6336u_read_touch(uint16_t *x, uint16_t *y, bool *pressed);

#ifdef __cplusplus
}
#endif
