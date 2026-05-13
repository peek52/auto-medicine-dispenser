#pragma once
#include <stddef.h>   // size_t
#include <time.h>     // struct tm
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ds3231 — RTC time module.
 * Address: ADDR_DS3231 (0x68)
 */

/** Read current time from DS3231. Returns formatted "HH:MM:SS" string. */
esp_err_t ds3231_get_time_str(char *buf, size_t buf_len);

/** Read current date from DS3231. Returns formatted "Www DD/MM/YYYY" string.
 *  e.g. "Sun 22/02/2026". Returns empty string on error. */
esp_err_t ds3231_get_date_str(char *buf, size_t buf_len);

/** Set the DS3231 time from a tm struct */
esp_err_t ds3231_set_time(struct tm *timeinfo);

/** Read the RTC and seed the ESP32 system time (settimeofday). Call
 *  once at boot AFTER i2c_manager_init() so the display has a real
 *  year before SNTP completes. Returns ESP_OK if seed succeeded. */
esp_err_t ds3231_seed_system_time(void);

#ifdef __cplusplus
}
#endif
