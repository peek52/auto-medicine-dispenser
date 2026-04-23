#pragma once
#include "esp_err.h"
#include "esp_cam_sensor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise MIPI-CSI2 camera, ISP, JPEG HW encoder and start capture task */
esp_err_t camera_init(void);

// เปิด/ปิดการแสดง log ของแต่ละเฟรมภาพ (เพื่อลดความรกบน monitor)
void camera_toggle_log(bool enable);
esp_cam_sensor_device_t *camera_get_sensor(void);
const char *camera_get_sensor_name(void);
esp_err_t camera_set_hmirror(bool enable);
esp_err_t camera_set_vflip(bool enable);
esp_err_t camera_set_brightness(int value);
esp_err_t camera_set_contrast(int value);
esp_err_t camera_set_saturation(int value);
bool camera_get_hmirror(void);
bool camera_get_vflip(void);
int camera_get_brightness(void);
int camera_get_contrast(void);
int camera_get_saturation(void);

#ifdef __cplusplus
}
#endif
