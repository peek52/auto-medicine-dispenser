# 🔖 Backup Points & Restore Guide

ไฟล์อ้างอิงสำหรับย้อนกลับมาที่จุด backup ใดก็ได้ ค้นหาด้วย Ctrl+F.

---

## 🌟 Recommended restore points (เรียงจากใหม่สุด)

| Tag | Commit | วันที่ | สถานะระบบ |
|-----|--------|-------|----------|
| **`stable-backup`** ⭐ | `e3ae430` | 2026-05-01 | TCA9548A + VL53 + camera + dispenser + touch ทำงานครบ |
| `working-pre-idf-downgrade` | (เก่า) | — | ก่อน downgrade IDF v5.3.3→v5.3.2 |
| `working-v532-no-camera` | (เก่า) | — | IDF v5.3.2, กล้องยังไม่ทำงาน |

> **ใช้แท็กไหน?** `stable-backup` คือจุดที่ดีที่สุด ณ ปัจจุบัน — ทุกฟีเจอร์ทำงาน

---

## 🚀 Quick restore commands

### A) ดูโค้ด ณ backup (ไม่ทับ branch)
```bash
git checkout stable-backup
```
จะอยู่ใน detached HEAD — ทดลองได้, กลับ branch เดิม:
```bash
git checkout fix/cloud-networking-bugs
```

### B) ทับ branch ปัจจุบันให้เป็น backup (ลบงานหลังจากนี้)
```bash
# สำรอง branch ปัจจุบันก่อน
git branch backup-$(date +%Y%m%d-%H%M)

# reset hard
git reset --hard stable-backup
```

### C) สร้าง branch ใหม่จาก backup
```bash
git checkout -b experiment-from-backup stable-backup
```

### D) ดึง tag จาก GitHub ถ้าหายในเครื่อง
```bash
git fetch --tags
git checkout stable-backup
```

---

## 🛠️ Build + flash หลัง restore

```bash
# Clean rebuild (จำเป็นถ้า managed_components เปลี่ยน)
idf.py -B build fullclean
idf.py -B build build
idf.py -B build -p COM6 flash
```

⚠️ **ถ้า managed_components ถูก update/reset:**
```bash
# Re-apply patches ที่เก็บไว้:
# (ดูใน patches/ folder)
git diff stable-backup -- patches/
```

---

## 📋 What's in `stable-backup` (e3ae430)

### Working features
- ✅ **Camera OV5647** — PID 0x5647 detect, MJPEG stream, /capture, Telegram /photo
- ✅ **TCA9548A + VL53L0X** (ch0 tested) — distance reading + pill count sync
- ✅ **Dispenser** — strict IR mode, return-all forces shadow=0, message matches audio
- ✅ **Touch FT6336U** — responsive, no phantom touches, tap-spam guard
- ✅ **NETPIE shadow sync, Telegram bot, Google Sheets, web UI**
- ✅ **I2C bus** — 0 INVALID_STATE warnings (root retry wrapper)

### Critical fixes
- IDF v5.3.3 i2c_master state-machine workaround (root level)
- OV5647 LDO 2.5→2.8 V (correct AVDD)
- SCCB INVALID_STATE retry (managed component patch)
- 12 bug fixes from code audit

### Files patched (in `patches/`)
- `patches/sccb_i2c_invalid_state_retry.patch` — managed_components
- `patches/ov5647_pid_relax.patch` — managed_components
- `i2c_master_null_guard_v533.patch` — IDF tree

---

## 🌐 Remote location

- **GitHub:** https://github.com/peek52/auto-medicine-dispenser
- **Branch:** `fix/cloud-networking-bugs`
- **Tag:** [`stable-backup`](https://github.com/peek52/auto-medicine-dispenser/releases/tag/stable-backup)

---

## 🆕 Create new backup point

เวลาทำงานเสร็จเป็น milestone อยากแบ็กอัพเพิ่ม:

```bash
# 1. Commit งานทั้งหมด
git add main/ patches/ *.md *.html
git commit -m "Backup: <describe what works>"

# 2. ตั้ง tag ชื่อสื่อความหมาย
git tag -a stable-<date>-<feature> -m "<description>" HEAD
# ตัวอย่าง:
#   git tag -a stable-2026-05-15-vl53-multi -m "All 6 VL53 sensors wired"

# 3. push
git push origin <branch> <tag-name>

# 4. แก้ BACKUPS.md (ไฟล์นี้) เพิ่มแถวในตาราง
```

---

## 🔍 ค้นหา commit/tag ด้วยคำว่า

```bash
# หา commit ที่มีคำว่า "TCA"
git log --oneline --grep="TCA"

# หา tag ทั้งหมด
git tag -l

# ดู commits 10 อันล่าสุดพร้อมกราฟ
git log --oneline --graph --all -10
```
