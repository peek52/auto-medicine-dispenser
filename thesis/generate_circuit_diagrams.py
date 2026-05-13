"""
Generate 3 diagrams for Chapter 3:
  - power_tree.png      (Figure 3-5, §3.6.1)
  - bus_tree.png        (Figure 3-6, §3.6.2)
  - task_hierarchy.png  (Figure 3-7, §3.7.1)

Usage:
    python thesis/generate_circuit_diagrams.py
"""

import os
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Rectangle

plt.rcParams['font.family'] = 'Tahoma'
plt.rcParams['font.size'] = 10
plt.rcParams['axes.unicode_minus'] = False

OUT_DIR = os.path.dirname(__file__)


def box(ax, x, y, w, h, text, color, text_color='white', fontsize=10, bold=True):
    b = FancyBboxPatch((x - w/2, y - h/2), w, h,
                       boxstyle='round,pad=0.05,rounding_size=0.15',
                       facecolor=color, edgecolor='black', linewidth=1.2)
    ax.add_patch(b)
    weight = 'bold' if bold else 'normal'
    ax.text(x, y, text, ha='center', va='center',
            fontsize=fontsize, fontweight=weight, color=text_color)


def arrow(ax, x1, y1, x2, y2, label='', color='black', lw=1.2,
          label_offset=(0.0, 0.0), fontsize=9):
    a = FancyArrowPatch((x1, y1), (x2, y2),
                        arrowstyle='->', mutation_scale=14,
                        color=color, linewidth=lw,
                        shrinkA=5, shrinkB=5)
    ax.add_patch(a)
    if label:
        mx = (x1 + x2) / 2 + label_offset[0]
        my = (y1 + y2) / 2 + label_offset[1]
        ax.text(mx, my, label, ha='center', va='center', fontsize=fontsize,
                color=color, fontweight='bold',
                bbox=dict(facecolor='white', edgecolor=color,
                          boxstyle='round,pad=0.2', linewidth=0.6))


# ════════════════════════════════════════════════════════════════════════════
# Figure 3-5 — Power Tree
# ════════════════════════════════════════════════════════════════════════════
def make_power_tree():
    fig, ax = plt.subplots(figsize=(14, 10))
    ax.set_xlim(0, 14)
    ax.set_ylim(0, 11)
    ax.set_aspect('equal')
    ax.axis('off')

    ax.text(7, 10.5, 'Power Tree — การกระจายแหล่งจ่ายไฟของระบบ',
            ha='center', fontsize=14, fontweight='bold')

    # AC mains (top center)
    box(ax, 7, 9.6, 2.4, 0.55, 'AC Mains 220V', '#212121')

    # Two switching PSUs side-by-side
    box(ax, 4.0, 8.5, 3.0, 0.7, 'Switching PSU 5V 10A', '#D32F2F')
    box(ax, 10.0, 8.5, 3.0, 0.7, 'Switching PSU 12V 2A', '#5E35B1')

    arrow(ax, 6.4, 9.3, 4.6, 8.85)
    arrow(ax, 7.6, 9.3, 9.4, 8.85)

    # ── 5V rail consumers (5 branches) ─────────────────────────────────────
    rail_y_5v = 7.4
    box(ax, 1.0, rail_y_5v, 1.8, 0.7, '5V → PCA9685\n+ Servo SG90 ×6', '#FF6F00',
        fontsize=8.5)
    box(ax, 3.0, rail_y_5v, 1.8, 0.7, '5V → VL53L0X×6\n(LDO ในโมดูล)', '#FF6F00',
        fontsize=8.5)
    box(ax, 5.0, rail_y_5v, 1.8, 0.7, '5V → I²C\nperipherals*', '#FF6F00',
        fontsize=8.5)
    box(ax, 7.0, rail_y_5v, 1.8, 0.7, '5V → ST7796S\nTFT (4″)', '#FF6F00',
        fontsize=8.5)
    box(ax, 9.0, rail_y_5v, 1.8, 0.7, '5V → ESP32-P4-Nano\n(VIN)', '#FF6F00',
        fontsize=8.5)

    arrow(ax, 3.5, 8.15, 1.3, 7.75, color='#D32F2F', lw=1.3)
    arrow(ax, 3.7, 8.15, 3.0, 7.75, color='#D32F2F', lw=1.3)
    arrow(ax, 4.0, 8.15, 5.0, 7.75, color='#D32F2F', lw=1.3)
    arrow(ax, 4.3, 8.15, 7.0, 7.75, color='#D32F2F', lw=1.3)
    arrow(ax, 4.5, 8.15, 8.7, 7.75, color='#D32F2F', lw=1.3)

    # ── 12V rail consumer (audio module only) ──────────────────────────────
    box(ax, 11.5, 7.4, 2.0, 0.7, '12V → DY-HV20T\n+ ลำโพง 8Ω 20W', '#7E57C2',
        fontsize=8.5)
    arrow(ax, 10.0, 8.15, 11.5, 7.75, color='#5E35B1', lw=1.3)

    # Footnote for I²C peripherals box
    ax.text(5.0, 6.6,
            '* PCF8574, TCA9548A,\n  DS3231, FT6336U\n  (ทุกโมดูลรับ 5V ได้ผ่าน LDO)',
            ha='center', va='center', fontsize=7.5, style='italic',
            color='#D32F2F')

    # Current notes
    ax.text(1.0, 6.6, '~4 A surge\n(servo 6× stall)',
            ha='center', va='center', fontsize=7.5, style='italic', color='#D32F2F')
    ax.text(3.0, 6.6, '~120 mA\n(VL53×6 active)',
            ha='center', va='center', fontsize=7.5, style='italic', color='#D32F2F')
    ax.text(11.5, 6.6, '~1.5 A peak\n(audio amp)',
            ha='center', va='center', fontsize=7.5, style='italic', color='#5E35B1')

    # ── ESP32 on-board LDO → 3.3V ──────────────────────────────────────────
    box(ax, 9.0, 5.4, 2.6, 0.65, 'On-board LDO\n(ESP32-P4-Nano)', '#1976D2',
        fontsize=9)
    arrow(ax, 9.0, 7.05, 9.0, 5.75, color='#FF6F00')

    # 3.3V internal rail (logic + I²C pull-ups inside MCU)
    box(ax, 9.0, 4.2, 3.0, 0.65, '3.3V Digital (internal)\n+ I²C Pull-ups 2.2 kΩ',
        '#388E3C', fontsize=9)
    arrow(ax, 9.0, 5.05, 9.0, 4.55, color='#1976D2', lw=1.2,
          label='3.3V', label_offset=(0.3, 0.0), fontsize=8)

    # 3.3V → camera LDO
    box(ax, 12.5, 4.2, 1.3, 0.65, 'LDO 2.8V', '#7B1FA2', fontsize=9)
    arrow(ax, 10.55, 4.2, 11.85, 4.2, color='#1976D2', lw=1.2,
          label='3.3V→LDO', label_offset=(0.0, 0.2), fontsize=7.5)

    # 2.8V → camera AVDD
    box(ax, 12.5, 3.0, 1.3, 0.85, '2.8V\nOV5647 AVDD', '#9575CD',
        text_color='white', fontsize=9)
    arrow(ax, 12.5, 3.85, 12.5, 3.45, color='#7B1FA2', lw=1.2)

    # Camera digital power (on the same 5V rail since module accepts it)
    ax.text(9.0, 3.4,
            '(I²C/SPI/MIPI logic ของ MCU\nใช้ 3.3V ภายในชิป)',
            ha='center', va='center', fontsize=7.5, style='italic', color='#388E3C')

    # ── Bottom note ────────────────────────────────────────────────────────
    ax.text(7, 1.3,
            'หมายเหตุการเลือกแหล่งจ่ายไฟ:\n'
            '• PSU 5V 10A — รองรับ servo stall (6 × 700 mA = 4.2 A) + I²C/TFT/MCU + margin\n'
            '• PSU 12V 2A แยก — สำหรับ DY-HV20T audio amp ที่ต้องการ 12V เพื่อขับลำโพง 20 W\n'
            '• LDO 2.8V บนบอร์ดกล้อง — แปลง 3.3V → AVDD ของ OV5647 ตามสเปก datasheet',
            ha='center', va='center', fontsize=9,
            bbox=dict(facecolor='#FFF9C4', edgecolor='#F57F17',
                      boxstyle='round,pad=0.5', linewidth=0.8))

    out = os.path.join(OUT_DIR, 'power_tree.png')
    plt.savefig(out, dpi=300, bbox_inches='tight', facecolor='white')
    print(f'Saved: {out}')
    plt.close()


# ════════════════════════════════════════════════════════════════════════════
# Figure 3-6 — Bus Tree
# ════════════════════════════════════════════════════════════════════════════
def make_bus_tree():
    fig, ax = plt.subplots(figsize=(14, 10))
    ax.set_xlim(0, 14)
    ax.set_ylim(0, 11)
    ax.set_aspect('equal')
    ax.axis('off')

    ax.text(7, 10.5, 'Bus Tree — การจัดการบัสสื่อสารทั้งหมดของระบบ',
            ha='center', fontsize=14, fontweight='bold')

    # Center: ESP32-P4-Nano
    box(ax, 7, 9.0, 4, 0.8, 'ESP32-P4-Nano (HP-core 360 MHz)', '#1976D2',
        fontsize=12)

    # ── I²C bus (left) ─────────────────────────────────────────────────────
    box(ax, 2.5, 7.5, 3.0, 0.7,
        'I²C0 @ 50 kHz\nSDA=GPIO7, SCL=GPIO8', '#388E3C', fontsize=9)
    arrow(ax, 5.5, 8.7, 3.5, 7.85, color='#388E3C', lw=1.5,
          label='I²C', label_offset=(-0.3, 0.2))

    # I²C devices (left column)
    dev_x = 0.8
    i2c_devices = [
        (6.4, 'TCA9548A\n0x70 (mux)'),
        (5.5, 'PCA9685\n0x40 (PWM)'),
        (4.6, 'PCF8574\n0x20 (IR exp.)'),
        (3.7, 'DS3231\n0x68 (RTC)'),
        (2.8, 'FT6336U\n0x38 (touch)'),
        (1.9, 'OV5647 SCCB\n0x36 (cam ctrl)'),
    ]
    for y, label in i2c_devices:
        box(ax, dev_x, y, 2.0, 0.6, label, '#66BB6A',
            text_color='black', fontsize=8.5, bold=False)
        arrow(ax, 2.5, 7.15, 1.8, y + 0.2, color='#388E3C', lw=0.7)

    # TCA9548A → 6× VL53L0X downstream
    box(ax, 5.0, 6.4, 2.0, 0.6, 'VL53L0X ×6\n(ch 0-5, all 0x29)',
        '#81C784', text_color='black', fontsize=8.5, bold=False)
    arrow(ax, dev_x + 1.0, 6.4, 4.0, 6.4, color='#388E3C', lw=0.9,
          label='ch 0-5', label_offset=(0, 0.15), fontsize=8)

    # Pull-up resistors note
    ax.text(2.5, 0.9,
            '2.2 kΩ pull-up บน SDA/SCL\nรองรับสาย 30-40 ซม. ที่ 50 kHz',
            ha='center', va='center', fontsize=8, style='italic',
            color='#388E3C',
            bbox=dict(facecolor='#E8F5E9', edgecolor='#388E3C',
                      boxstyle='round,pad=0.3'))

    # ── SPI bus (top right) ────────────────────────────────────────────────
    box(ax, 9.5, 7.5, 3.0, 0.7,
        'SPI2 @ 40 MHz mode 0', '#F57C00', fontsize=9)
    arrow(ax, 8.5, 8.7, 9.0, 7.85, color='#F57C00', lw=1.5,
          label='SPI', label_offset=(0.3, 0.2))

    box(ax, 9.5, 6.4, 3.0, 0.7,
        'ST7796S TFT 480×320\nMOSI=32 SCK=36 CS=26\nDC=24 RST=25',
        '#FFB74D', text_color='black', fontsize=8.5, bold=False)
    arrow(ax, 9.5, 7.15, 9.5, 6.75, color='#F57C00')

    # ── MIPI-CSI ───────────────────────────────────────────────────────────
    box(ax, 13.0, 7.5, 1.7, 0.7, 'MIPI-CSI\n2 lanes', '#7B1FA2', fontsize=9)
    arrow(ax, 8.7, 8.7, 12.4, 7.85, color='#7B1FA2', lw=1.5)

    box(ax, 13.0, 6.4, 1.7, 0.7,
        'OV5647\n5 MP camera', '#BA68C8', text_color='black',
        fontsize=8.5, bold=False)
    arrow(ax, 13.0, 7.15, 13.0, 6.75, color='#7B1FA2')
    ax.text(13.0, 5.6, 'XCLK 24 MHz\n→ GPIO33',
            ha='center', va='center', fontsize=8, style='italic', color='#7B1FA2')

    # ── UART (bottom right) ────────────────────────────────────────────────
    box(ax, 9.5, 4.5, 3.0, 0.7,
        'UART1 @ 9600 8-N-1', '#C62828', fontsize=9)
    arrow(ax, 8.0, 8.6, 9.0, 4.85, color='#C62828', lw=1.5,
          label='UART', label_offset=(0.4, 0.0))

    box(ax, 9.5, 3.4, 3.0, 0.7,
        'DY-HV20T audio\nTX=GPIO37 (RX unused)',
        '#EF5350', text_color='white', fontsize=8.5, bold=False)
    arrow(ax, 9.5, 4.15, 9.5, 3.75, color='#C62828')

    # ── SDIO (bottom right) ────────────────────────────────────────────────
    box(ax, 13.0, 4.5, 1.7, 0.7, 'SDIO 4-bit\n40 MHz', '#0097A7', fontsize=9)
    arrow(ax, 9.0, 8.6, 12.4, 4.85, color='#0097A7', lw=1.5)

    box(ax, 13.0, 3.4, 1.7, 0.7,
        'ESP32-C6\n(WiFi co-processor)', '#26C6DA',
        text_color='black', fontsize=8.5, bold=False)
    arrow(ax, 13.0, 4.15, 13.0, 3.75, color='#0097A7')
    ax.text(13.0, 2.5, 'GPIO14-19\nESP-Hosted',
            ha='center', va='center', fontsize=8, style='italic',
            color='#0097A7')

    # ── GPIO direct (XSHUT lines) ──────────────────────────────────────────
    box(ax, 7.0, 4.5, 3.0, 0.7,
        'GPIO ตรง — XSHUT control', '#5D4037', fontsize=9)
    arrow(ax, 7.0, 8.6, 7.0, 4.85, color='#5D4037', lw=1.2,
          label='6× GPIO', label_offset=(0.3, 0.5))
    box(ax, 7.0, 3.4, 3.0, 0.7,
        'XSHUT VL53×6\nGPIO 20,22,23,47,48,53',
        '#A1887F', text_color='white', fontsize=8.5, bold=False)
    arrow(ax, 7.0, 4.15, 7.0, 3.75, color='#5D4037')

    out = os.path.join(OUT_DIR, 'bus_tree.png')
    plt.savefig(out, dpi=300, bbox_inches='tight', facecolor='white')
    print(f'Saved: {out}')
    plt.close()


# ════════════════════════════════════════════════════════════════════════════
# Figure 3-7 — Task Hierarchy (FreeRTOS)
# ════════════════════════════════════════════════════════════════════════════
def make_task_hierarchy():
    fig, ax = plt.subplots(figsize=(13, 9))
    ax.set_xlim(0, 13)
    ax.set_ylim(0, 10)
    ax.axis('off')

    ax.text(6.5, 9.5,
            'FreeRTOS Task Hierarchy — การจัดลำดับ task ของระบบ',
            ha='center', fontsize=14, fontweight='bold')
    ax.text(6.5, 9.0,
            '(เรียงตาม priority สูง → ต่ำ; priority สูงทำงานก่อน — preemptive scheduling)',
            ha='center', fontsize=9, style='italic', color='#616161')

    tasks = [
        # (priority, name, stack, core, role, group_color)
        (7, 'cam_task',         8192, 'Core 0', 'รับเฟรม MIPI-CSI + ISP + JPEG encode', '#7B1FA2'),
        (5, 'tg_poll',          12288, 'any', 'Long-poll Telegram getUpdates ทุก 3s', '#0288D1'),
        (5, 'tg_pho_task',      12288, 'any', 'ส่งภาพ JPEG ผ่าน HTTPS sendPhoto', '#0288D1'),
        (5, 'tg_text_wrk',      10240, 'any', 'ส่งข้อความ Telegram จาก queue', '#0288D1'),
        (5, 'vl53_task',        8192, 'any', 'Poll VL53×6 ผ่าน TCA9548A mux', '#388E3C'),
        (5, 'sync_time',        4096, 'any', 'SNTP sync เวลากับ time server', '#0288D1'),
        (4, 'dispenser',        8192, 'any', 'Scheduler RTC + จ่ายยา + รอ confirm 15 นาที', '#D32F2F'),
        (4, 'offline_flush',    8192, 'any', 'Replay queue ที่ส่งคลาวด์ไม่สำเร็จ', '#0288D1'),
        (4, 'tg_report',        12288, 'any', 'รายงาน periodic ผ่าน Telegram', '#0288D1'),
        (3, 'clk_task',         10240, 'any', 'วาดจอ TFT + อ่าน touch + UI logic', '#F57C00'),
        (3, 'offline_watch',    4096, 'any', 'Watch network state, trigger flush', '#0288D1'),
        (3, 'idle_music',       3072, 'any', 'เล่นเสียง idle ผ่าน DY-HV20T', '#F57C00'),
        (2, 'i2c_wdog',         4096, 'any', 'I²C bus health monitor + recovery', '#5D4037'),
        (1, 'liveness',         4096, 'any', 'Feed TWDT ทุก 2 วินาที (§2.3.2)', '#5D4037'),
        (0, 'IDLE0/IDLE1',      0, 'core', 'FreeRTOS idle (subscribed to TWDT)', '#9E9E9E'),
    ]

    # Header
    headers = ['Priority', 'Task Name', 'Stack', 'Core', 'หน้าที่']
    col_x = [0.4, 1.7, 4.1, 5.0, 6.3]
    col_w = [1.2, 2.3, 0.8, 0.8, 6.4]
    header_y = 8.3

    for i, h in enumerate(headers):
        ax.add_patch(Rectangle((col_x[i], header_y - 0.25), col_w[i], 0.5,
                                facecolor='#37474F', edgecolor='black',
                                linewidth=0.8))
        ax.text(col_x[i] + col_w[i]/2, header_y, h,
                ha='center', va='center', fontsize=10,
                color='white', fontweight='bold')

    # Rows
    row_h = 0.45
    for i, (prio, name, stack, core, role, color) in enumerate(tasks):
        y = header_y - 0.55 - i * row_h
        # Priority badge with color
        ax.add_patch(Rectangle((col_x[0], y - row_h/2 + 0.05), col_w[0], row_h - 0.1,
                                facecolor=color, edgecolor='black',
                                linewidth=0.5, alpha=0.85))
        ax.text(col_x[0] + col_w[0]/2, y, str(prio),
                ha='center', va='center', fontsize=10,
                color='white', fontweight='bold')

        # Task name (mono-style)
        ax.text(col_x[1] + 0.1, y, name,
                ha='left', va='center', fontsize=9.5,
                family='Consolas' if name != 'IDLE0/IDLE1' else 'Tahoma')

        # Stack
        stack_str = str(stack) if stack > 0 else '—'
        ax.text(col_x[2] + col_w[2]/2, y, stack_str,
                ha='center', va='center', fontsize=9)

        # Core
        ax.text(col_x[3] + col_w[3]/2, y, core,
                ha='center', va='center', fontsize=9)

        # Role
        ax.text(col_x[4] + 0.1, y, role,
                ha='left', va='center', fontsize=9)

        # Row separator
        ax.plot([col_x[0], col_x[4] + col_w[4]],
                [y - row_h/2, y - row_h/2],
                color='lightgrey', lw=0.3)

    # Legend (color groups)
    legend_y = 0.7
    legend_items = [
        ('#7B1FA2', 'Vision pipeline'),
        ('#388E3C', 'Sensor polling'),
        ('#D32F2F', 'Dispenser core'),
        ('#0288D1', 'Cloud / network'),
        ('#F57C00', 'UI + audio'),
        ('#5D4037', 'Watchdog / health'),
    ]
    for i, (color, label) in enumerate(legend_items):
        x = 0.5 + i * 2.1
        ax.add_patch(Rectangle((x, legend_y - 0.15), 0.3, 0.3,
                                facecolor=color, edgecolor='black', linewidth=0.5))
        ax.text(x + 0.4, legend_y, label, fontsize=8.5, va='center')

    out = os.path.join(OUT_DIR, 'task_hierarchy.png')
    plt.savefig(out, dpi=300, bbox_inches='tight', facecolor='white')
    print(f'Saved: {out}')
    plt.close()


if __name__ == '__main__':
    make_power_tree()
    make_bus_tree()
    make_task_hierarchy()
    print('All 3 diagrams generated.')
