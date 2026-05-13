"""
Generate Cloud Architecture diagram for thesis Section 3.5.1.

Shows ESP32-P4-Nano connecting to 3 cloud services + 1 LAN web UI.

Output: thesis/cloud_architecture.png  (300 DPI)

Usage:
    python thesis/generate_cloud_architecture.py
"""

import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
import os

plt.rcParams['font.family'] = 'Tahoma'
plt.rcParams['font.size'] = 10
plt.rcParams['axes.unicode_minus'] = False

# Colors
ESP_COLOR     = '#1976D2'   # blue (center MCU)
NETPIE_COLOR  = '#4CAF50'   # green (NETPIE)
TG_COLOR      = '#0288D1'   # light blue (Telegram brand)
GAS_COLOR     = '#E53935'   # red (Google Apps Script)
WEB_COLOR     = '#FB8C00'   # orange (Web UI LAN)
LOCK_COLOR    = '#616161'   # grey

fig, ax = plt.subplots(figsize=(13, 9))
ax.set_xlim(0, 13)
ax.set_ylim(0, 9)
ax.set_aspect('equal')
ax.axis('off')

# ── Title ─────────────────────────────────────────────────────────────────
ax.text(6.5, 8.5, 'สถาปัตยกรรมการเชื่อมต่อระหว่างเครื่องและบริการคลาวด์',
        ha='center', fontsize=14, fontweight='bold')

# ── Helper for service boxes ──────────────────────────────────────────────
def service_box(x, y, w, h, title, subtitle, color):
    box = FancyBboxPatch((x - w/2, y - h/2), w, h,
                         boxstyle='round,pad=0.05,rounding_size=0.2',
                         facecolor=color, edgecolor='black', linewidth=1.5,
                         alpha=0.9)
    ax.add_patch(box)
    ax.text(x, y + 0.25, title, ha='center', va='center',
            fontsize=11, fontweight='bold', color='white')
    ax.text(x, y - 0.25, subtitle, ha='center', va='center',
            fontsize=9, color='white')

# ── Center: ESP32-P4-Nano ─────────────────────────────────────────────────
center_x, center_y = 6.5, 4.5
esp_w, esp_h = 3.0, 1.5
box = FancyBboxPatch((center_x - esp_w/2, center_y - esp_h/2), esp_w, esp_h,
                     boxstyle='round,pad=0.05,rounding_size=0.2',
                     facecolor=ESP_COLOR, edgecolor='black', linewidth=2,
                     alpha=0.95)
ax.add_patch(box)
ax.text(center_x, center_y + 0.4, 'ESP32-P4-Nano', ha='center', va='center',
        fontsize=13, fontweight='bold', color='white')
ax.text(center_x, center_y, 'เครื่องจ่ายยาอัตโนมัติ', ha='center', va='center',
        fontsize=10, color='white')
ax.text(center_x, center_y - 0.4, '(IoT Edge Device)', ha='center', va='center',
        fontsize=9, color='white', style='italic')

# ── 4 corners: cloud services ─────────────────────────────────────────────
# Top-left: NETPIE
nx, ny = 2.0, 7.0
service_box(nx, ny, 2.8, 1.2, 'NETPIE 2020', 'MQTT broker (Shadow)', NETPIE_COLOR)

# Top-right: Telegram
tx, ty = 11.0, 7.0
service_box(tx, ty, 2.8, 1.2, 'Telegram Bot', 'api.telegram.org', TG_COLOR)

# Bottom-left: Google Apps Script
gx, gy = 2.0, 2.0
service_box(gx, gy, 2.8, 1.2, 'Google Sheets', 'Apps Script Webhook', GAS_COLOR)

# Bottom-right: Web Browser (LAN)
wx, wy = 11.0, 2.0
service_box(wx, wy, 2.8, 1.2, 'Web Browser', 'หน้าเว็บภายใน LAN', WEB_COLOR)

# ── Connection lines with labels and lock icons ───────────────────────────
def draw_connection(x1, y1, x2, y2, label, color, locked=True, dash=False):
    style = '--' if dash else '-'
    arrow = FancyArrowPatch((x1, y1), (x2, y2),
                             arrowstyle='<->', mutation_scale=18,
                             color=color, linewidth=2.2, linestyle=style,
                             shrinkA=10, shrinkB=10)
    ax.add_patch(arrow)
    # Midpoint for label
    mx, my = (x1 + x2) / 2, (y1 + y2) / 2
    # Label box
    ax.text(mx, my, label, ha='center', va='center',
            fontsize=9, fontweight='bold', color=color,
            bbox=dict(facecolor='white', edgecolor=color,
                     boxstyle='round,pad=0.3', linewidth=1))
    # Lock icon
    if locked:
        # Offset the lock slightly toward the cloud service
        dx, dy = (x2 - x1), (y2 - y1)
        length = (dx**2 + dy**2)**0.5
        # Position lock at 35% along the line
        lock_x = x1 + dx * 0.35
        lock_y = y1 + dy * 0.35
        ax.text(lock_x, lock_y + 0.25, '[TLS]', ha='center', va='center',
                fontsize=8, fontweight='bold', color='white',
                bbox=dict(facecolor=LOCK_COLOR, edgecolor='none',
                         boxstyle='round,pad=0.2'))

# Connections
draw_connection(center_x - 1.2, center_y + 0.5, nx + 0.8, ny - 0.4,
                'MQTT 1883', NETPIE_COLOR)
draw_connection(center_x + 1.2, center_y + 0.5, tx - 0.8, ty - 0.4,
                'HTTPS 443', TG_COLOR)
draw_connection(center_x - 1.2, center_y - 0.5, gx + 0.8, gy + 0.4,
                'HTTPS POST', GAS_COLOR)
draw_connection(center_x + 1.2, center_y - 0.5, wx - 0.8, wy + 0.4,
                'HTTP 80', WEB_COLOR, locked=False)

# ── Side annotations for each channel ─────────────────────────────────────
ax.text(2.0, 5.7, '[1] ซิงก์สถานะอุปกรณ์\n      แบบเรียลไทม์',
        ha='center', va='center', fontsize=8.5, color=NETPIE_COLOR,
        style='italic')

ax.text(11.0, 5.7, '[2] แจ้งเตือนผู้ดูแล\n      ส่งภาพ + ข้อความ',
        ha='center', va='center', fontsize=8.5, color=TG_COLOR,
        style='italic')

ax.text(2.0, 3.3, '[3] บันทึก audit log\n      ลง Google Sheets',
        ha='center', va='center', fontsize=8.5, color=GAS_COLOR,
        style='italic')

ax.text(11.0, 3.3, '[4] ตั้งค่าผ่านหน้าเว็บ\n      (LAN เท่านั้น)',
        ha='center', va='center', fontsize=8.5, color=WEB_COLOR,
        style='italic')

# ── Legend at bottom ──────────────────────────────────────────────────────
ax.text(6.5, 0.7, '[TLS] = HTTPS / TLS 1.2 encrypted    |    <-> = bidirectional communication',
        ha='center', va='center', fontsize=9, color='black',
        bbox=dict(facecolor='#F5F5F5', edgecolor='grey',
                 boxstyle='round,pad=0.4', linewidth=0.5))

# ── Save ──────────────────────────────────────────────────────────────────
out_path = os.path.join(os.path.dirname(__file__), 'cloud_architecture.png')
plt.savefig(out_path, dpi=300, bbox_inches='tight', facecolor='white')
print(f'Saved: {out_path}')
plt.close()
