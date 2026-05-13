#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

typedef struct {
    char name[32];
    int count;
    uint8_t slots;
    int home_angle;
    int work_angle;
} netpie_med_t;

typedef struct {
    bool enabled;
    char slot_time[7][6];
    netpie_med_t med[DISPENSER_MED_COUNT];
    bool loaded;
} netpie_shadow_t;

void netpie_init(void);
void netpie_shadow_get(void);
void netpie_shadow_update_count(int med_id, int new_count);
void netpie_shadow_update_med_name(int med_id, const char *name);
void netpie_shadow_update_med_slots(int med_id, uint8_t slots_mask);
void netpie_shadow_update_slot(int slot_idx, const char *hh_mm);
void netpie_shadow_update_enabled(bool enabled);

bool netpie_shadow_copy(netpie_shadow_t *out_shadow);
const netpie_shadow_t *netpie_get_shadow(void);
bool netpie_is_connected(void);
bool netpie_publish_shadow_json(const char *payload);
uint32_t netpie_mqtt_get_last_rx_time(void);

#ifdef __cplusplus
}
#endif
