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
 * Returns true if the task was created (or was already running). */
bool bridge_vl53_start(void);

/* Copy the latest 6 distance readings into out_mm[] (length must be
 * BRIDGE_VL53_NUM_SENSORS). Returns the millisecond-since-boot tick of
 * the last successful parse — 0 if no line has been received yet. */
uint32_t bridge_vl53_get(int *out_mm);

/* GET /vl53 — auto-refreshing HTML dashboard showing the 6 latest
 * distances + a "last update X s ago" heartbeat. No authentication. */
esp_err_t bridge_vl53_html_handler(httpd_req_t *req);

/* GET /vl53.json — JSON snapshot for the page's 500 ms fetch loop. */
esp_err_t bridge_vl53_json_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
