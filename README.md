# Auto Medicine Dispenser

ESP-IDF firmware for an ESP32-P4 based automatic pill dispenser with touchscreen UI, NETPIE shadow sync, web control, camera snapshot support, and Telegram notifications.

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

Typical local command used in this repo:

```powershell
cmd /c "call C:\Users\peekz\esp\esp-idf\export.bat && idf.py -B build_idf build"
```

Flash example:

```powershell
cmd /c "call C:\Users\peekz\esp\esp-idf\export.bat && idf.py -B build_idf -p COM6 flash"
```

## Helpful Git Commands

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

- `AI_CONTEXT.md` contains a handoff-style project summary for teammates and AI tools.
- A local `repomix-output.xml` can be generated when needed with:

```powershell
npx repomix@latest
```

That generated file is intentionally not committed to Git.
