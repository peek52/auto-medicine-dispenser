# Backup — Camera Working State

**Date**: 2026-05-07
**Git commit**: `c33bc77` ("Fix MIPI-CSI camera: get OV5647 frames flowing reliably on ESP32-P4")
**Branch when backed up**: `fix/cloud-networking-bugs`

## What's in here

- `main/` — full source snapshot of the firmware app code
- `sdkconfig`, `sdkconfig.defaults` — Kconfig that includes the AF_ENABLE=n fix
- `CMakeLists.txt`, `partitions.csv` — build config
- `unified_cam.bin`, `bootloader.bin`, `partition-table.bin` — pre-built binaries (flashable as-is)

## Camera state at this snapshot

OV5647 streams at 36-50 fps reliably after auto-recovery handles the ~20-40% of cold boots where DPHY initial lock fails. Fix details in commit message and `memory/project_camera_status.md`.

## How to restore

### Restore source via git (preferred)

```powershell
git checkout backup-camera-working-2026-05-07
# or use the branch:
git checkout backup/camera-working-2026-05-07
```

### Hard reset current branch back to this commit

```powershell
git reset --hard c33bc77
```

### Restore source from this folder if git is broken

```powershell
# from D:/project/ddddddddd/unified_cam
Copy-Item -Recurse -Force backup_camera_working_c33bc77/main .
Copy-Item -Force backup_camera_working_c33bc77/sdkconfig .
Copy-Item -Force backup_camera_working_c33bc77/sdkconfig.defaults .
```

### Flash the saved binary directly (no build needed)

```powershell
cmd /c "call C:\Users\peekz\esp\esp-idf\export.bat && python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x2000 backup_camera_working_c33bc77\bootloader.bin 0x8000 backup_camera_working_c33bc77\partition-table.bin 0x10000 backup_camera_working_c33bc77\unified_cam.bin"
```
