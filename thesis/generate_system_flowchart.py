"""
Generate System Flowchart for thesis Section 3.4 — readable version.

Output: thesis/system_flowchart.png  (300 DPI)
"""

import os
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Polygon

plt.rcParams['font.family'] = 'Tahoma'
plt.rcParams['axes.unicode_minus'] = False

# ── Colors (high contrast) ────────────────────────────────────────────────
START_COLOR = '#2E7D32'      # dark green
END_COLOR   = '#1565C0'      # blue
PROC_COLOR  = '#90CAF9'      # light blue
DEC_COLOR   = '#FFD54F'      # bright yellow
CLOUD_COLOR = '#BA68C8'      # purple
ERROR_COLOR = '#EF5350'      # red

SUCCESS_LINE = '#2E7D32'
MISSED_LINE  = '#C62828'

# ── Layout: wider canvas + more vertical spacing ──────────────────────────
fig, ax = plt.subplots(figsize=(14, 22))
ax.set_xlim(0, 14)
ax.set_ylim(0, 30)
ax.axis('off')
ax.set_aspect('equal')

# Title
ax.text(7, 29.0, 'แผนผังการทำงานของระบบเครื่องจ่ายยาอัตโนมัติ',
        ha='center', fontsize=18, fontweight='bold')


# ── Helpers ───────────────────────────────────────────────────────────────
def oval(x, y, w, h, text, color, fs=13):
    box = FancyBboxPatch((x - w/2, y - h/2), w, h,
                         boxstyle='round,pad=0.1,rounding_size=0.6',
                         facecolor=color, edgecolor='black', linewidth=1.8)
    ax.add_patch(box)
    ax.text(x, y, text, ha='center', va='center',
            fontsize=fs, fontweight='bold', color='white')


def rect(x, y, w, h, text, color=PROC_COLOR, fs=12, text_color='black'):
    box = FancyBboxPatch((x - w/2, y - h/2), w, h,
                         boxstyle='round,pad=0.05,rounding_size=0.2',
                         facecolor=color, edgecolor='black', linewidth=1.4)
    ax.add_patch(box)
    ax.text(x, y, text, ha='center', va='center',
            fontsize=fs, color=text_color)


def diamond(x, y, w, h, text, fs=12):
    pts = [(x, y + h/2), (x + w/2, y), (x, y - h/2), (x - w/2, y)]
    poly = Polygon(pts, facecolor=DEC_COLOR, edgecolor='black', linewidth=1.6)
    ax.add_patch(poly)
    ax.text(x, y, text, ha='center', va='center',
            fontsize=fs, fontweight='bold')


def arr(x1, y1, x2, y2, color='black', lw=2.0, label='', label_pos='right',
        label_fs=12):
    a = FancyArrowPatch((x1, y1), (x2, y2),
                        arrowstyle='->', mutation_scale=22,
                        color=color, linewidth=lw,
                        shrinkA=2, shrinkB=2)
    ax.add_patch(a)
    if label:
        mx, my = (x1 + x2) / 2, (y1 + y2) / 2
        if label_pos == 'right':
            mx += 0.5
        elif label_pos == 'left':
            mx -= 0.5
        elif label_pos == 'above':
            my += 0.3
        ax.text(mx, my, label, ha='center', va='center',
                fontsize=label_fs, fontweight='bold', color=color,
                bbox=dict(facecolor='white', edgecolor=color,
                          boxstyle='round,pad=0.4', linewidth=1.2))


# ── Nodes (centered column at x=7, big spacing) ───────────────────────────
W_PROC = 5.0
H_PROC = 1.0

# 1. Start
oval(7, 28.0, 3.5, 1.0, 'เริ่มต้นระบบ', START_COLOR)

# 2. Init
rect(7, 26.4, W_PROC, H_PROC,
     'Initialize: WiFi, NETPIE,\nI²C bus, sensors, camera')

# 3. RTC poll
rect(7, 24.7, W_PROC, H_PROC,
     'อ่านเวลาจาก RTC DS3231\n(โพลทุก 10 วินาที)')

# 4. Decision: dispense time?
diamond(7, 22.8, 3.6, 1.6, 'ถึงเวลา\nจ่ายยา?', fs=13)

# 5. Set waiting_confirm
rect(7, 20.7, W_PROC + 0.5, H_PROC,
     'ตั้งสถานะ waiting_confirm\nแสดง popup บนจอ + ส่ง Telegram')

# 6. Decision: confirmed?
diamond(7, 18.8, 4.6, 1.7, 'ผู้ใช้กดยืนยัน\nภายใน 15 นาที?', fs=12)

# ── SUCCESS PATH (left column) ────────────────────────────────────────────
LEFT_X = 2.8
rect(LEFT_X, 16.5, 4.5, 1.0,
     'Servo SG90 หมุน\nHome → Work → Home')
rect(LEFT_X, 14.8, 4.5, 1.0,
     'IR Sensor ตรวจจับเม็ดยาตก\n+ Debounce 30 ms')
rect(LEFT_X, 13.1, 4.5, 1.0,
     'VL53L0X อ่านระดับยา\nผ่าน TCA9548A mux')
rect(LEFT_X, 11.4, 4.5, 1.0,
     'OV5647 ถ่ายภาพ\n+ Hardware JPEG encode')

# Cloud sync (3 channels)
rect(LEFT_X, 9.4, 4.5, 1.1,
     '[1] NETPIE MQTT Publish\n@shadow/data/update', CLOUD_COLOR, text_color='white')
rect(LEFT_X, 7.8, 4.5, 1.1,
     '[2] Telegram sendPhoto\nรูปยา + จำนวน + เวลา', CLOUD_COLOR, text_color='white')
rect(LEFT_X, 6.2, 4.5, 1.1,
     '[3] Google Apps Script\nบันทึก audit log ลง Sheets', CLOUD_COLOR, text_color='white')

# ── MISSED PATH (right column) ────────────────────────────────────────────
RIGHT_X = 11.2
rect(RIGHT_X, 16.5, 4.6, 1.0,
     'Mark missed_slots_mask\nไม่จ่ายยาในมื้อนั้น', ERROR_COLOR, text_color='white')
rect(RIGHT_X, 14.4, 4.6, 1.3,
     'ส่ง Telegram แจ้ง "พลาดมื้อ"\nถึงผู้ดูแล + บันทึก\nGoogle Sheets', ERROR_COLOR, text_color='white')

# ── Merge ─────────────────────────────────────────────────────────────────
rect(7, 3.5, 6.0, 1.0,
     'รอรอบถัดไป — กลับสู่ scheduler loop')

# End
oval(7, 1.5, 4.0, 1.0, 'วนกลับขั้นที่ 3', END_COLOR)


# ── Arrows ────────────────────────────────────────────────────────────────
# Main column down
arr(7, 27.5, 7, 26.9)                                  # Start → Init
arr(7, 25.9, 7, 25.2)                                  # Init → RTC
arr(7, 24.2, 7, 23.6)                                  # RTC → decision

# Decision: yes ↓ / no ← loop back
arr(7, 22.0, 7, 21.2, label='ใช่', label_pos='right', label_fs=12)

# "ไม่" path: loop back from decision left side to RTC box
arr(5.2, 22.8, 0.7, 22.8, color='black')
arr(0.7, 22.8, 0.7, 24.7, color='black')
arr(0.7, 24.7, 4.5, 24.7, color='black')
ax.text(0.7, 23.7, 'ไม่', fontsize=13, fontweight='bold',
        ha='center', va='center',
        bbox=dict(facecolor='white', edgecolor='black',
                  boxstyle='round,pad=0.3', linewidth=1.0))

arr(7, 20.2, 7, 19.65)                                 # waiting → confirm

# Confirm: yes ↙ / no ↘
arr(5.0, 18.6, 3.5, 17.0,
    color=SUCCESS_LINE, lw=2.4, label='ยืนยันแล้ว',
    label_pos='left', label_fs=12)
arr(9.0, 18.6, 10.5, 17.0,
    color=MISSED_LINE, lw=2.4, label='หมดเวลา 15 นาที',
    label_pos='right', label_fs=11)

# Success path (left column down)
arr(LEFT_X, 16.0, LEFT_X, 15.3, color=SUCCESS_LINE, lw=1.8)
arr(LEFT_X, 14.3, LEFT_X, 13.6, color=SUCCESS_LINE, lw=1.8)
arr(LEFT_X, 12.6, LEFT_X, 11.9, color=SUCCESS_LINE, lw=1.8)
arr(LEFT_X, 10.9, LEFT_X, 10.0, color=SUCCESS_LINE, lw=1.8)
arr(LEFT_X, 8.85, LEFT_X, 8.35, color=SUCCESS_LINE, lw=1.8)
arr(LEFT_X, 7.25, LEFT_X, 6.75, color=SUCCESS_LINE, lw=1.8)

# Missed path (right column down)
arr(RIGHT_X, 16.0, RIGHT_X, 15.05, color=MISSED_LINE, lw=2.0)

# ── Converge to merge box top edge ────────────────────────────────────────
# Success: from cloud-3 box → diagonal to top-left of merge box
arr(LEFT_X, 5.65, LEFT_X, 4.6, color=SUCCESS_LINE, lw=2.0)
arr(LEFT_X, 4.6, 5.0, 4.0, color=SUCCESS_LINE, lw=2.0)

# Missed: long vertical drop on right side, then diagonal in
arr(RIGHT_X, 13.75, RIGHT_X, 4.6, color=MISSED_LINE, lw=2.0)
arr(RIGHT_X, 4.6, 9.0, 4.0, color=MISSED_LINE, lw=2.0)

# Merge → end
arr(7, 3.0, 7, 2.0)


# ── Legend (bottom, larger) ───────────────────────────────────────────────
legend_y = 0.0
legend_items = [
    (START_COLOR, 'เริ่ม/จบ'),
    (PROC_COLOR, 'กระบวนการ'),
    (DEC_COLOR, 'การตัดสินใจ'),
    (CLOUD_COLOR, 'การสื่อสารคลาวด์'),
    (ERROR_COLOR, 'เส้นทางพลาดมื้อ'),
]
for i, (color, label) in enumerate(legend_items):
    x = 0.8 + i * 2.65
    box = FancyBboxPatch((x, legend_y - 0.2), 0.5, 0.5,
                         boxstyle='round,pad=0.02',
                         facecolor=color, edgecolor='black', linewidth=1.0)
    ax.add_patch(box)
    ax.text(x + 0.7, legend_y + 0.05, label, fontsize=12, va='center')


# ── Save ──────────────────────────────────────────────────────────────────
out_path = os.path.join(os.path.dirname(__file__), 'system_flowchart.png')
plt.savefig(out_path, dpi=300, bbox_inches='tight', facecolor='white')
print(f'Saved: {out_path}')
plt.close()
