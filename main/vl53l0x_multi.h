#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start TCA9548A + VL53L0X initialization and polling task
void vl53l0x_multi_start(void);
void vl53l0x_multi_bootstrap(void);

// Sensor config — ปรับได้ผ่านเว็บ (per-channel)
void vl53l0x_set_channel_config(int ch, int full_dist_mm, int pill_height_mm, int max_pills);

#ifdef __cplusplus
}
#endif
