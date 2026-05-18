#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
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
    int  max_pills;     /* Per-module pill ceiling. Set via web/touch UI,
                         * persisted to NVS through the shadow. Falls back
                         * to compile-time DISPENSER_MAX_PILLS when shadow
                         * hasn't loaded yet or the saved value is bogus
                         * (<= 0 or > 999). Both the touch UI (+/- step)
                         * and the MQTT count-update clamper read this. */
    bool loaded;
} netpie_shadow_t;

void netpie_init(void);
void netpie_shadow_get(void);
void netpie_shadow_update_count(int med_id, int new_count);
void netpie_shadow_update_med_name(int med_id, const char *name);
void netpie_shadow_update_med_slots(int med_id, uint8_t slots_mask);
/* Returns true on commit. False if the proposed time would violate the
 * slot-time rules (malformed, pre >= post within a meal pair, OR
 * duplicates the HH:MM of another already-set slot). On false the
 * shadow is unchanged and no MQTT publish fires — the caller is
 * responsible for surfacing the rejection to the user. */
bool netpie_shadow_update_slot(int slot_idx, const char *hh_mm);

/* Atomic two-slot write — validates the merged state once and commits
 * both or neither. Use when a UI action changes both halves of a meal
 * pair together (cascade) so half-applied state can't leak through
 * the per-slot validator. Same accept rules as netpie_shadow_update_slot
 * applied to the merged state (pre<post, gap ≤ 60 min, no duplicates). */
bool netpie_shadow_update_slot_pair(int idx_a, const char *val_a,
                                    int idx_b, const char *val_b);

/* Returns true iff writing `hh_mm` to slot `slot_idx` would satisfy
 * the slot-time rules: pre < post within the same meal pair AND no
 * duplicate HH:MM among set slots. Callers (touch UI time picker, web
 * widget JS) use this to preview before committing. */
bool netpie_slot_time_valid(int slot_idx, const char *hh_mm);
void netpie_shadow_update_enabled(bool enabled);

bool netpie_shadow_copy(netpie_shadow_t *out_shadow);
const netpie_shadow_t *netpie_get_shadow(void);

/* Effective per-module pill ceiling. Returns the shadow's max_pills
 * field if loaded + sane (1..999), else falls back to the compile-time
 * DISPENSER_MAX_PILLS. Safe to call from any task; reads a volatile
 * scalar so no locking needed. */
int dispenser_max_pills(void);

/* Set max_pills locally + publish to NETPIE so the cloud and any other
 * subscribers pick up the change. Clamped to 1..999. Used by the /tech
 * web page so the new ceiling is effective immediately without waiting
 * for the MQTT round-trip. */
void netpie_shadow_update_max_pills(int new_max);

/* ── Pending-approval flow ─────────────────────────────────────────
 *
 * Shadow writes that arrive from outside (NETPIE web widget) are held
 * in a "pending" buffer instead of being applied immediately. The
 * operator must approve via the touch screen OR Telegram /approve
 * before the changes commit; /reject (or touch Cancel) republishes
 * the current shadow back to NETPIE, reverting the cloud state.
 *
 * Self-echoes from outbound publishes don't trigger this — they hit
 * the path where incoming values already match s_shadow exactly, so
 * no diff = no pending event. */

typedef struct {
    bool active;                 /* true while waiting for approval */
    /* Pending values (what the cloud just sent us). */
    netpie_shadow_t snapshot;
    /* What actually differs vs current s_shadow — only these fields
     * will be applied on approve. */
    bool enabled_diff;
    bool max_pills_diff;
    bool slot_time_diff[7];
    bool med_name_diff[DISPENSER_MED_COUNT];
    bool med_count_diff[DISPENSER_MED_COUNT];
    bool med_slots_diff[DISPENSER_MED_COUNT];
    uint32_t arrived_tick;       /* xTaskGetTickCount() at first arrival */
} netpie_pending_t;

/* True while pending-review is active. UI / Telegram poll this. */
bool netpie_pending_active(void);
/* Atomic copy of the pending state. Returns false if nothing pending. */
bool netpie_pending_get(netpie_pending_t *out);
/* Apply pending changes → s_shadow. NOP if nothing pending. */
void netpie_pending_approve(void);
/* Discard pending changes + republish current s_shadow so the cloud
 * reverts. NOP if nothing pending. */
void netpie_pending_reject(void);
/* One-shot "pending just activated" notice for the Telegram task.
 * Returns true exactly once per inactive→active transition (reading
 * clears the flag). Cleared on approve/reject so a stale notice from
 * a resolved review never fires. */
bool netpie_pending_take_telegram_notify(void);

/* Render a Thai-language diff summary of the current pending state
 * into out. Returns characters written (excluding NUL), or 0 if no
 * pending review is active. Truncates silently if out is too small. */
size_t netpie_pending_format_summary_th(char *out, size_t out_cap);
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
