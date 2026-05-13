#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * display_clock — ST7796S 480×320 standby clock via LovyanGFX
 * Shows HH:MM:SS, date, and IP from DS3231 RTC.
 */

/** Initialise SPI bus and ST7796S panel. Call once after I2C is ready. */
void display_clock_init(void);

/** Start the 1-second FreeRTOS refresh task. Call after WiFi connects. */
void display_clock_start_task(void);

/** Update the IP address shown at the bottom of the screen. */
void display_clock_set_ip(const char *ip);

#ifdef __cplusplus
}
#endif
