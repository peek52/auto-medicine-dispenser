#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PILL_SENSOR_COUNT 6

typedef struct {
    uint8_t address;
    bool present;
    bool valid;
    int raw_mm;
    int filtered_mm;
} pill_sensor_status_t;

void pill_sensor_status_init_defaults(void);
void pill_sensor_status_mark_present(int idx, bool present);
void pill_sensor_status_set_reading(int idx, int raw_mm, int filtered_mm, bool valid);
const pill_sensor_status_t *pill_sensor_status_get_all(void);

#ifdef __cplusplus
}
#endif
