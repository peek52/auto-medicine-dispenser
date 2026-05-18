#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only passive listener for the ESP32-C3 Super Mini distance-sensor
 * bridge. The external C3 owns the TCA9548A + 6 VL53L0X sensors and is
 * expected to stream a CSV line every poll cycle on its UART TX:
 *
 *   "vl53:<s0>,<s1>,<s2>,<s3>,<s4>,<s5>\n"  (the "vl53:" prefix is optional)
 *
 * Each <sN> is the latest distance reading in millimetres (integer).
 * -1 (or any value outside 0..9999) is treated as "no reading / out of
 * range" and surfaced as such on /vl53.
 *
 * IMPORTANT: this module is fully isolated from the dispense scheduler,
 * IR state machine, and any other production path. It only stores the
 * most recent values + a heartbeat tick for the web layer to display.
 * Do NOT wire it into dispense logic without an explicit user ask —
 * VL53 on the P4 bus was abandoned 2026-05-14. */

#define BRIDGE_VL53_NUM_SENSORS 6

/* Start the UART listener task. Idempotent; subsequent calls are no-ops.
 * Returns true if the task was created (or was already running). The
 * task always drains the UART (to keep the kernel buffer from filling)
 * but ONLY commits parsed values into the visible snapshot during the
 * 5-second window after bridge_vl53_arm_read() was called — so the
 * dashboard shows "--" at boot and the operator has to press the
 * "อ่านค่า" button before any numbers appear (user spec 2026-05-18:
 * "กดแล้วค่อยอ่าน อ่านครั้งละ 5 วิพอ"). */
bool bridge_vl53_start(void);

/* Open the 5-second read window. Subsequent CSV lines from the C3
 * get committed to the visible snapshot until the window expires;
 * after that the parser drops incoming values until the next arm.
 * Safe to call repeatedly — each call refreshes the window. */
void bridge_vl53_arm_read(void);

/* Copy the latest 6 distance readings into out_mm[] (length must be
 * BRIDGE_VL53_NUM_SENSORS). Returns the millisecond-since-boot tick of
 * the last committed parse — 0 if no read window has been armed yet. */
uint32_t bridge_vl53_get(int *out_mm);

/* Returns ms remaining in the current read window. 0 = not active. */
uint32_t bridge_vl53_remaining_ms(void);

/* GET /vl53 — HTML dashboard with an "อ่านค่า" button. */
esp_err_t bridge_vl53_html_handler(httpd_req_t *req);

/* GET /vl53.json — JSON snapshot for the page's polling loop. Includes
 * read-window state so the page knows when to stop polling. */
esp_err_t bridge_vl53_json_handler(httpd_req_t *req);

/* POST /vl53/read — arm a fresh 5-second read window. */
esp_err_t bridge_vl53_read_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
