#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start TCA9548A + VL53L0X initialization and polling task
void vl53l0x_multi_start(void);
void vl53l0x_multi_bootstrap(void);

// Cooperative pause for the VL53 poll task — used by camera_init() so the
// I2C bus is quiet during the OV5647 SCCB burst. Calls nest (each pause
// must be matched by a resume).
void vl53l0x_multi_pause(void);
void vl53l0x_multi_resume(void);

// Block until the VL53 task is actually idle (parked observing pause).
// Caller must have already called vl53l0x_multi_pause(). Used by camera
// init to ensure SCCB doesn't race with VL53 register reads.
void vl53l0x_multi_wait_idle(uint32_t timeout_ms);

#include <stdbool.h>
// True while the synchronous bootstrap (vl53_init_all) is running. Camera
// retry task waits for this to clear before pulling XSHUT pins low.
bool vl53l0x_multi_is_bootstrapping(void);

// On-demand poll trigger (fire-and-forget). Wakes the VL53 task to run
// ONE poll cycle; does NOT wait for completion. Use after dispense to
// refresh fill status in the background without blocking the caller.
void vl53l0x_request_refresh(void);

#include "esp_err.h"
// Blocking on-demand check — triggers one full poll cycle and waits up
// to timeout_ms for the task to finish all 6 channels (typically 2-5 s).
// Returns ESP_OK if completed, ESP_ERR_TIMEOUT if the task didn't
// finish (e.g. paused for camera SCCB). After return, fresh readings
// are available via pill_sensor_status_get(idx). Use this for the
// Telegram /status command and other user-triggered queries.
esp_err_t vl53l0x_multi_check_now(uint32_t timeout_ms);

// Sensor config — ปรับได้ผ่านเว็บ (per-channel)
void vl53l0x_set_channel_config(int ch, int full_dist_mm, int pill_height_mm, int max_pills);
void vl53l0x_set_channel_offset(int ch, int count_offset);
void vl53l0x_load_calibration_from_nvs(void);

#ifdef __cplusplus
}
#endif
