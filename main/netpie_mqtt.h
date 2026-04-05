#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  netpie_mqtt.h — NETPIE 2020 MQTT client (Shadow read/write)
//
//  การใช้งาน:
//    1. netpie_init()        — เรียกหลัง WiFi connected
//    2. netpie_shadow_get()  — ขอ shadow ครั้งแรก (auto-called ใน init)
//    3. shadow data จะถูก cache ใน netpie_shadow_t — อ่านผ่าน netpie_get_shadow()
//    4. netpie_shadow_update_count(id, n) — อัปเดตจำนวนยาหลังจ่าย
// ─────────────────────────────────────────────────────────────────────────────

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/* ── ข้อมูลยา 1 ตลับ ── */
typedef struct {
    char     name[32];   // ชื่อยา
    int      count;      // จำนวนเม็ดที่เหลือ (0-16)
    uint8_t  slots;      // bitmask 7-bit: bit0=เช้า-ก่อน … bit6=นอน
    int      home_angle; // ตำแหน่ง Servo ตอนอยู่เฉยๆ (Default: 10)
    int      work_angle; // ตำแหน่ง Servo ตอนผลักยา (Default: 90)
} netpie_med_t;

/* ── ข้อมูล schedule ทั้งหมดจาก shadow ── */
typedef struct {
    bool  enabled;                          // scheduleEnabled
    char  slot_time[7][6];                  // "HH:MM" สำหรับ slot 0-6
    netpie_med_t med[DISPENSER_MED_COUNT];  // ยา 6 ตลับ (index 0-5)
    bool  loaded;                           // shadow ได้รับครั้งแรกแล้ว?
} netpie_shadow_t;

/* ── Public API ── */

/** เริ่ม MQTT client และเชื่อม NETPIE */
void netpie_init(void);

/** ส่ง request ขอ shadow data ใหม่ */
void netpie_shadow_get(void);

/** อัปเดต count ของยา med_id (1-6) และ publish ไปที่ shadow */
void netpie_shadow_update_count(int med_id, int new_count);

/** อัปเดตชื่อยา med_id (1-6) และ publish ไปที่ shadow */
void netpie_shadow_update_med_name(int med_id, const char *name);

/** อัปเดต slots (bitmask) ของยา med_id (1-6) และ publish ไปที่ shadow */
void netpie_shadow_update_med_slots(int med_id, uint8_t slots_mask);

uint32_t netpie_mqtt_get_last_rx_time(void);

void netpie_shadow_update_slot(int slot_idx, const char *hh_mm);
void netpie_shadow_update_enabled(bool enabled);
bool netpie_shadow_copy(netpie_shadow_t *out_shadow);
bool netpie_publish_shadow_json(const char *payload);

/** คืน pointer ไปยัง shadow data ที่ cached (อ่านได้ ห้าม write โดยตรง) */
const netpie_shadow_t *netpie_get_shadow(void);

/** true = connected กับ NETPIE broker */
bool netpie_is_connected(void);

#ifdef __cplusplus
}
#endif
