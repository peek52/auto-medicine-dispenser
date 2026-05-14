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

/* Publish-inhibit: while pushed, all outbound shadow MQTT publishes
 * are dropped silently — local shadow / NVS still update. Used by the
 * meds-detail UI to batch +/- / slot / rename edits and publish once
 * via netpie_shadow_commit_med_diff() when the user exits the page.
 * Nest-safe: every push must be matched by exactly one pop. */
void netpie_publish_inhibit_push(void);
void netpie_publish_inhibit_pop(void);

/* Compare the current shadow for med_id (1..N) against the supplied
 * backup and publish only the fields that differ (name / count /
 * slots). Idempotent if nothing changed — sends zero MQTT messages.
 * Call AFTER netpie_publish_inhibit_pop() so the diff messages actually
 * make it out. */
void netpie_shadow_commit_med_diff(int med_id, const netpie_med_t *backup);
uint32_t netpie_mqtt_get_last_rx_time(void);

#ifdef __cplusplus
}
#endif
