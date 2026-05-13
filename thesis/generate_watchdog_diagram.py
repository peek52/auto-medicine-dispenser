"""
Generate Three-Tier Watchdog timing diagram for thesis Section 2.3.2.

Output: thesis/watchdog_timing.png  (300 DPI, Thai labels)

Usage:
    pip install matplotlib
    python thesis/generate_watchdog_diagram.py
"""

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyArrowPatch, Rectangle
import os

# ── Thai font setup (Windows) ──────────────────────────────────────────────
plt.rcParams['font.family'] = 'Tahoma'  # Windows built-in, supports Thai
plt.rcParams['font.size'] = 11
plt.rcParams['axes.unicode_minus'] = False

FEED_INTERVAL = 2       # canary feeds every 2 s
TWDT_TIMEOUT = 30       # TWDT timeout in seconds

GREEN = '#4CAF50'
YELLOW = '#FFC107'
RED = '#E53935'
BLUE = '#1976D2'
GREY = '#757575'

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 7), constrained_layout=True)

# ════════════════════════════════════════════════════════════════════════════
# CASE A — Normal operation
# ════════════════════════════════════════════════════════════════════════════
ax1.set_xlim(-1, 22)
ax1.set_ylim(-1.5, 3.5)
ax1.set_title('(ก) สภาวะปกติ — canary task feed TWDT ทุก 2 วินาที',
              fontsize=12, fontweight='bold', loc='left')

# Feed arrows every 2 s
for t in range(0, 21, FEED_INTERVAL):
    ax1.annotate('', xy=(t, 1.0), xytext=(t, 2.0),
                 arrowprops=dict(arrowstyle='->', color=GREEN, lw=1.8))
    ax1.text(t, 2.2, '↓feed', ha='center', fontsize=8, color=GREEN)

# TWDT bar — keeps resetting (shown as repeating green segments)
for t in range(0, 21, FEED_INTERVAL):
    end = min(t + FEED_INTERVAL, 21)
    ax1.add_patch(Rectangle((t, -0.3), end - t, 0.8,
                             facecolor=GREEN, edgecolor='white',
                             linewidth=1, alpha=0.7))
ax1.text(10, 0.1, 'TWDT countdown รีเซ็ตทุกครั้งที่ feed → ระบบเสถียร',
         ha='center', va='center', fontsize=10, color='white', fontweight='bold')

# Labels
ax1.text(-0.8, 1.5, 'Canary\ntask',  ha='right', va='center', fontsize=10, color=GREEN)
ax1.text(-0.8, 0.1, 'TWDT\ntimer',   ha='right', va='center', fontsize=10, color=BLUE)

# Time axis
ax1.axhline(y=-0.7, color='black', lw=0.5)
for t in range(0, 21, 2):
    ax1.plot([t, t], [-0.75, -0.65], 'k-', lw=0.5)
    ax1.text(t, -1.0, f'{t}', ha='center', fontsize=9)
ax1.text(21.5, -1.0, 'เวลา (วินาที)', ha='left', fontsize=9, style='italic')

ax1.axis('off')

# ════════════════════════════════════════════════════════════════════════════
# CASE B — Scheduler stalled
# ════════════════════════════════════════════════════════════════════════════
ax2.set_xlim(-1, 38)
ax2.set_ylim(-1.5, 3.5)
ax2.set_title('(ข) สภาวะผิดปกติ — scheduler ค้าง, canary หยุด feed → TWDT trigger panic ที่ 30 วินาที',
              fontsize=12, fontweight='bold', loc='left')

LAST_FEED = 4   # last successful feed at t=4s
PANIC_AT = LAST_FEED + TWDT_TIMEOUT   # = 34 s

# Successful feeds (before stall)
for t in range(0, LAST_FEED + 1, FEED_INTERVAL):
    ax2.annotate('', xy=(t, 1.0), xytext=(t, 2.0),
                 arrowprops=dict(arrowstyle='->', color=GREEN, lw=1.8))
    ax2.text(t, 2.2, '↓feed', ha='center', fontsize=8, color=GREEN)

# Stall marker
ax2.plot(LAST_FEED + 1, 1.5, 'X', color=RED, markersize=18, mew=3)
ax2.text(LAST_FEED + 1, 2.5, 'scheduler\nค้าง',
         ha='center', va='bottom', fontsize=9, color=RED, fontweight='bold')

# Missed feeds (red dashed arrows showing feeds that should have happened)
for t in range(LAST_FEED + FEED_INTERVAL, PANIC_AT, FEED_INTERVAL):
    ax2.annotate('', xy=(t, 1.0), xytext=(t, 1.8),
                 arrowprops=dict(arrowstyle='->', color=RED, lw=1.0,
                                 linestyle='dashed', alpha=0.4))

# TWDT bar — countdown from green → yellow → red across 30 s
n_segs = 60
seg_w = TWDT_TIMEOUT / n_segs
for i in range(n_segs):
    t = LAST_FEED + i * seg_w
    frac = i / n_segs
    if frac < 0.5:
        color = GREEN
    elif frac < 0.8:
        color = YELLOW
    else:
        color = RED
    ax2.add_patch(Rectangle((t, -0.3), seg_w, 0.8,
                             facecolor=color, edgecolor='none', alpha=0.85))

# Pre-stall TWDT bar (normal green resets)
for t in range(0, LAST_FEED, FEED_INTERVAL):
    ax2.add_patch(Rectangle((t, -0.3), FEED_INTERVAL, 0.8,
                             facecolor=GREEN, edgecolor='white',
                             linewidth=1, alpha=0.7))

# Countdown timer label
ax2.text(LAST_FEED + 15, 0.1, '30-second countdown — ไม่มีการ feed',
         ha='center', va='center', fontsize=10, color='white', fontweight='bold')

# PANIC marker
ax2.axvline(x=PANIC_AT, color=RED, lw=2, linestyle='--', alpha=0.7)
ax2.plot(PANIC_AT, 1.5, '*', color=RED, markersize=28)
ax2.annotate('PANIC!\nesp_task_wdt_isr()\n→ system reboot',
             xy=(PANIC_AT, 1.5), xytext=(PANIC_AT + 1.5, 2.6),
             fontsize=9, color=RED, fontweight='bold',
             arrowprops=dict(arrowstyle='->', color=RED, lw=1.2))

# Labels
ax2.text(-0.8, 1.5, 'Canary\ntask',  ha='right', va='center', fontsize=10, color=GREEN)
ax2.text(-0.8, 0.1, 'TWDT\ntimer',   ha='right', va='center', fontsize=10, color=BLUE)

# Time axis
ax2.axhline(y=-0.7, color='black', lw=0.5)
for t in range(0, 38, 2):
    ax2.plot([t, t], [-0.75, -0.65], 'k-', lw=0.5)
    if t % 4 == 0 or t == PANIC_AT:
        ax2.text(t, -1.0, f'{t}', ha='center', fontsize=9,
                 fontweight='bold' if t == PANIC_AT else 'normal',
                 color=RED if t == PANIC_AT else 'black')
ax2.text(38.5, -1.0, 'เวลา (วินาที)', ha='left', fontsize=9, style='italic')

ax2.axis('off')

# ── Legend (figure-level) ──────────────────────────────────────────────────
legend_elems = [
    mpatches.Patch(facecolor=GREEN, label='TWDT countdown ปกติ (เพิ่งถูก reset)'),
    mpatches.Patch(facecolor=YELLOW, label='TWDT ใกล้หมดเวลา'),
    mpatches.Patch(facecolor=RED, label='TWDT ใกล้ trigger panic'),
    plt.Line2D([0], [0], marker=r'$\downarrow$', color=GREEN, lw=0,
               markersize=12, label='esp_task_wdt_reset() จาก canary'),
    plt.Line2D([0], [0], marker='*', color=RED, lw=0,
               markersize=15, label='Panic handler → reboot'),
]
fig.legend(handles=legend_elems, loc='lower center', ncol=5,
           fontsize=9, frameon=False, bbox_to_anchor=(0.5, -0.02))

# ── Save ───────────────────────────────────────────────────────────────────
out_path = os.path.join(os.path.dirname(__file__), 'watchdog_timing.png')
plt.savefig(out_path, dpi=300, bbox_inches='tight', facecolor='white')
print(f'Saved: {out_path}')
plt.close()
