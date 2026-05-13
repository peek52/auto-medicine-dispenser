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

// On-demand poll trigger — bypasses the normal 5 s sleep and clears all
// retry-after timers so missing channels get re-probed immediately.
// Call after dispense / return-pill / count-adjust events when a fresh
// reading matters; the task otherwise idles to keep the I2C bus quiet.
void vl53l0x_request_refresh(void);

// Sensor config — ปรับได้ผ่านเว็บ (per-channel)
void vl53l0x_set_channel_config(int ch, int full_dist_mm, int pill_height_mm, int max_pills);
void vl53l0x_set_channel_offset(int ch, int count_offset);
void vl53l0x_load_calibration_from_nvs(void);

#ifdef __cplusplus
}
#endif
