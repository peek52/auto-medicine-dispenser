#include "pill_sensor_status.h"

static pill_sensor_status_t s_sensors[PILL_SENSOR_COUNT] = {
    { 0x71, false, false, -1, -1 },
    { 0x72, false, false, -1, -1 },
    { 0x73, false, false, -1, -1 },
    { 0x74, false, false, -1, -1 },
    { 0x75, false, false, -1, -1 },
    { 0x76, false, false, -1, -1 },
};

void pill_sensor_status_init_defaults(void)
{
    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        s_sensors[i].address = (uint8_t)(0x71 + i);
        s_sensors[i].present = false;
        s_sensors[i].valid = false;
        s_sensors[i].raw_mm = -1;
        s_sensors[i].filtered_mm = -1;
    }
}

void pill_sensor_status_mark_present(int idx, bool present)
{
    if (idx < 0 || idx >= PILL_SENSOR_COUNT) return;
    s_sensors[idx].present = present;
    if (!present) {
        s_sensors[idx].valid = false;
        s_sensors[idx].raw_mm = -1;
        s_sensors[idx].filtered_mm = -1;
    }
}

void pill_sensor_status_set_reading(int idx, int raw_mm, int filtered_mm, bool valid)
{
    if (idx < 0 || idx >= PILL_SENSOR_COUNT) return;
    s_sensors[idx].raw_mm = raw_mm;
    s_sensors[idx].filtered_mm = filtered_mm;
    s_sensors[idx].valid = valid;
}

const pill_sensor_status_t *pill_sensor_status_get_all(void)
{
    return s_sensors;
}
