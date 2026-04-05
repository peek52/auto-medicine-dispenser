# AI Context

This file is a quick handoff summary for teammates or AI tools that need project context fast.

## สรุปภาษาไทย

ไฟล์นี้เป็นสรุปโปรเจกต์แบบอ่านเร็ว สำหรับเพื่อนร่วมทีม อาจารย์ หรือ AI ตัวอื่นที่ต้องเข้าใจโครงสร้างโปรเจกต์ก่อนแก้โค้ด

### ภาพรวมโปรเจกต์

โปรเจกต์นี้เป็นเฟิร์มแวร์ของเครื่องจ่ายยาอัตโนมัติบน ESP32-P4 โดยรวมระบบต่อไปนี้ไว้ด้วยกัน:

- หน้าจอสัมผัสบนตัวเครื่อง
- หน้าเว็บสำหรับดูสถานะและควบคุมบางส่วน
- การซิงก์ข้อมูลกับ NETPIE MQTT Shadow
- การแจ้งเตือนและรับคำสั่งผ่าน Telegram
- กล้องสำหรับถ่ายภาพหรือส่ง snapshot
- ระบบเก็บคิวงาน offline เพื่อรอส่งข้อมูลเมื่อเน็ตกลับมา

แม้ชื่อโปรเจกต์ในโค้ดคือ `unified_cam` แต่ในการใช้งานจริงคือระบบเครื่องจ่ายยาอัตโนมัติพร้อมกล้องและระบบคลาวด์

### ฮาร์ดแวร์และโมดูลหลัก

- ชิปหลัก ESP32-P4
- จอขับผ่าน LovyanGFX
- Touch controller: FT6336U
- RTC: DS3231
- Servo driver: PCA9685
- I/O expander: PCF8574
- กล้องพร้อม JPEG pipeline
- มีการเตรียมรองรับ DFPlayer ในเฟิร์มแวร์

### ลำดับการเริ่มทำงาน

ไฟล์หลักคือ `main/main.c`

1. เริ่ม NVS
2. โหลด cloud secrets และสถานะ offline sync
3. เริ่ม shared I2C
4. เปิด background init task
5. จากนั้นใน deferred init จะ:
   - โหลด settings
   - เริ่ม Wi-Fi
   - sync เวลา SNTP
   - เริ่ม NETPIE
   - เริ่มกล้อง
   - เริ่ม web server และ stream server
   - เริ่มตัว scheduler สำหรับจ่ายยา
   - เริ่ม USB mouse
   - เริ่ม CLI task

### ไฟล์สำคัญที่ควรอ่านก่อน

- `main/main.c`
- `main/dispenser_scheduler.c`
- `main/netpie_mqtt.c`
- `main/offline_sync.c`
- `main/telegram_bot.c`
- `main/web_server.c`
- `main/web_handlers_status.c`
- `main/web_handlers_stream.c`
- `main/ui_standby.cpp`
- `main/ui_setup_schedule.cpp`
- `main/ui_setup_meds.cpp`
- `netpie_dashboard_copy.html`

### พฤติกรรมสำคัญปัจจุบัน

- NETPIE Shadow เก็บสถานะเปิด/ปิดตาราง เวลาแต่ละช่วง ชื่อยา จำนวนยา และ slot mask
- หน้า standby มี popup แจ้งเตือนตารางยาและ hardware warning
- popup hardware warning ปัจจุบัน:
  - แสดงเฉพาะโมดูลที่มีปัญหา
  - ลดอาการกระพริบแล้ว
  - หายเองเมื่อ hardware กลับมาปกติ
- ทั้งหน้าจอสัมผัสและ dashboard ฝั่ง NETPIE ใช้โมเดลข้อมูลยาและเวลาเดียวกัน

### จุดประสงค์ของไฟล์นี้

ถ้าจะให้ AI ตัวอื่นช่วยต่อ ควรให้อ่าน `README.md` และไฟล์นี้ก่อน เพื่อเข้าใจโครงสร้างโปรเจกต์และไฟล์สำคัญที่ควรเปิดอ่านก่อนแก้

## Project Summary

This repository contains firmware for an automatic pill dispenser built on ESP32-P4. The system combines:

- A touchscreen device UI
- Local web control and diagnostics
- NETPIE MQTT shadow sync
- Telegram notifications and commands
- Camera snapshot support
- Offline queueing for cloud retries

The project name is `unified_cam`, but functionally it is an automatic medicine dispenser system with camera and cloud features.

## Hardware/Subsystem Overview

- ESP32-P4 main controller
- Display driven through LovyanGFX
- Touch controller: FT6336U
- RTC: DS3231
- Servo driver: PCA9685
- I/O expander: PCF8574
- Camera pipeline with JPEG support
- DFPlayer support prepared in firmware

## Startup Flow

High-level startup is in `main/main.c`.

1. Initialize NVS
2. Load cloud secrets and offline sync state
3. Initialize shared I2C bus
4. Start background init task
5. In deferred init:
   - load settings
   - start Wi-Fi
   - sync time with SNTP
   - init NETPIE
   - init camera
   - start web server and stream server
   - start dispenser scheduler
   - start USB mouse
   - start CLI task

## Important Source Files

- `main/main.c`
  Main startup and service boot order.
- `main/dispenser_scheduler.c`
  Core dispensing behavior, reminders, stock decrement, and alert flow.
- `main/netpie_mqtt.c`
  NETPIE broker connection and shadow cache/update logic.
- `main/offline_sync.c`
  Retry queues for shadow payloads, Telegram text, and Google Sheets events.
- `main/telegram_bot.c`
  Telegram polling, command handling, text/photo sending, and language switching.
- `main/web_server.c`
  Starts HTTP server and registers routes.
- `main/web_handlers_status.c`
  Status JSON and admin/diagnostic web endpoints.
- `main/web_handlers_stream.c`
  Camera state and stream controls.
- `main/ui_standby.cpp`
  Standby screen, popups, next-dose view, and hardware warning popup.
- `main/ui_setup_schedule.cpp`
  Touch UI for medicine schedule configuration.
- `main/ui_setup_meds.cpp`
  Touch UI for medicine names, counts, and slot assignments.
- `netpie_dashboard_copy.html`
  NETPIE dashboard HTML widget for cloud-side schedule and cartridge editing.

## Current Behavior Notes

- NETPIE shadow stores schedule enable state, dose times, medicine names, pill counts, and slot masks.
- UI standby screen includes popups for next schedule and hardware warnings.
- Hardware warning popup was recently adjusted to:
  - show only failing modules
  - stay stable without flicker
  - disappear automatically when hardware recovers
- Touchscreen UI and dashboard both work with the same medicine/timing concepts.

## Web / Cloud Features

- Web server exposes:
  - status
  - camera controls
  - stream controls
  - maintenance/admin routes
- NETPIE is used as the main cloud shadow state store.
- Telegram can send alerts, snapshots, and accept commands such as status/help/lang.
- Offline sync keeps pending outbound actions for later retry.

## Build Notes

- Project uses ESP-IDF with `components/LovyanGFX`.
- In this repo, builds have commonly used `build_idf` as the build directory.
- Typical local flash target seen in development: `COM6`

## Suggested Prompt For Other AI Tools

If you give this repo to another AI, a good short prompt is:

```text
This is an ESP-IDF firmware project for an ESP32-P4 automatic pill dispenser with touchscreen UI, NETPIE MQTT shadow sync, Telegram bot integration, web diagnostics, camera support, and offline retry queues. Please read README.md and AI_CONTEXT.md first, then inspect main/main.c, main/dispenser_scheduler.c, main/netpie_mqtt.c, main/offline_sync.c, main/telegram_bot.c, main/web_server.c, and main/ui_standby.cpp before making changes.
```
