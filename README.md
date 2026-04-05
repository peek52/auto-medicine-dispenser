# Auto Medicine Dispenser

ESP-IDF firmware for an ESP32-P4 based automatic pill dispenser with touchscreen UI, NETPIE shadow sync, web control, camera snapshot support, and Telegram notifications.

## คำอธิบายภาษาไทย

โปรเจกต์นี้คือเฟิร์มแวร์สำหรับเครื่องจ่ายยาอัตโนมัติบนบอร์ด ESP32-P4 โดยมีทั้งหน้าจอสัมผัส, ระบบซิงก์ข้อมูลผ่าน NETPIE, หน้าเว็บสำหรับควบคุมและตรวจสอบสถานะ, ระบบกล้อง, และการแจ้งเตือนผ่าน Telegram

### โปรเจกต์นี้ทำอะไรได้บ้าง

- ควบคุมตลับยา 6 ช่อง
- เก็บชื่อยา จำนวนยา และเวลาจ่ายยาไว้ใน NETPIE Shadow
- แสดงผลและตั้งค่าผ่านหน้าจอสัมผัสบนตัวเครื่อง
- มีหน้าเว็บสำหรับดูสถานะ กล้อง และงานซ่อมบำรุง
- แจ้งเตือนหรือส่งข้อมูลผ่าน Telegram
- มีระบบคิวรอส่งข้อมูลเมื่ออินเทอร์เน็ตล่ม แล้วค่อยส่งซ้ำภายหลัง

### ไฟล์สำคัญในโปรเจกต์

- `main/main.c`
  จุดเริ่มต้นของระบบ การเปิด service ต่าง ๆ และลำดับการเริ่มทำงาน
- `main/ui_*.cpp`
  ไฟล์หน้าจอสัมผัส เช่น หน้าหลัก เมนู ตั้งค่า Wi-Fi ตั้งเวลาจ่ายยา และตั้งค่าตลับยา
- `main/dispenser_scheduler.c`
  ตรรกะหลักของการจ่ายยา การเตือน และการอัปเดตจำนวนยาคงเหลือ
- `main/netpie_mqtt.c`
  ส่วนเชื่อมต่อ NETPIE MQTT และอ่าน/เขียน Shadow
- `main/offline_sync.c`
  ส่วนเก็บคิวงานที่ต้องส่งขึ้นคลาวด์ภายหลัง
- `main/telegram_bot.c`
  ส่วนส่งข้อความ รูปภาพ และรับคำสั่งจาก Telegram
- `main/web_server.c`
  เริ่มต้น HTTP server
- `main/web_handlers_*.c`
  API ของหน้าเว็บ เช่น status, stream, servo, camera
- `main/camera_init.c`
  ส่วนเริ่มต้นกล้อง ISP และ JPEG pipeline
- `components/LovyanGFX`
  ไลบรารีที่ใช้ขับจอแสดงผล

## What This Project Does

- Controls a 6-slot medicine dispenser.
- Stores medicine names, counts, and time slots in NETPIE shadow.
- Shows a touchscreen UI on the device display.
- Runs a local web interface for status, camera, and maintenance controls.
- Sends medication and alert messages through Telegram.
- Keeps offline sync queues for cloud actions when the network is unavailable.

## Main Parts Of The Project

- `main/main.c`
  System startup, NVS init, deferred init task, CLI task, and service startup.
- `main/ui_*.cpp`
  Touchscreen UI screens such as standby, menu, settings, Wi-Fi, medicine setup, and schedule setup.
- `main/dispenser_scheduler.c`
  Medication timing logic, dispense flow, alerts, and stock updates.
- `main/netpie_mqtt.c`
  NETPIE MQTT connection and shadow read/write logic.
- `main/offline_sync.c`
  Queues pending cloud work and retries later.
- `main/telegram_bot.c`
  Telegram commands, text alerts, and photo sending.
- `main/web_server.c`
  HTTP server startup and route registration.
- `main/web_handlers_*.c`
  Web API endpoints for status, stream, and servo/camera controls.
- `main/camera_init.c`
  Camera, ISP, JPEG, and capture pipeline setup.
- `components/LovyanGFX`
  Display library used by the UI.

## Build

This project uses ESP-IDF and the extra component directory in `components/`.

คำสั่ง build ที่ใช้บ่อยในเครื่องนี้:

Typical local command used in this repo:

```powershell
cmd /c "call C:\Users\peekz\esp\esp-idf\export.bat && idf.py -B build_idf build"
```

Flash example:

```powershell
cmd /c "call C:\Users\peekz\esp\esp-idf\export.bat && idf.py -B build_idf -p COM6 flash"
```

## Helpful Git Commands

คำสั่ง Git ที่ใช้บ่อย:

After making changes:

```powershell
git add .
git commit -m "Describe your change"
git push
```

View history:

```powershell
git log --oneline
```

See local unstaged changes:

```powershell
git diff
```

## For AI / Project Handoff

สำหรับส่งต่อให้เพื่อนหรือ AI:

- `README.md` ใช้อ่านภาพรวมโปรเจกต์
- `AI_CONTEXT.md` ใช้เป็นสรุปเชิงเทคนิคแบบละเอียดขึ้น
- ถ้าต้องการรวมไฟล์สำคัญเป็นก้อนเดียวสำหรับ AI ให้สร้าง `repomix-output.xml`

- `AI_CONTEXT.md` contains a handoff-style project summary for teammates and AI tools.
- A local `repomix-output.xml` can be generated when needed with:

```powershell
npx repomix@latest
```

That generated file is intentionally not committed to Git.
