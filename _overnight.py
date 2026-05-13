import serial, time, os, sys
PORT = "COM6"
LOG = r"D:\project\ddddddddd\unified_cam\overnight.log"
DURATION_S = 28800  # 8 hours
MARKER_EVERY_S = 60

start = time.time()
end = start + DURATION_S
last_marker = 0
with open(LOG, "ab") as f:
    f.write(f"\n\n===== OVERNIGHT CAPTURE START {time.strftime('%Y-%m-%d %H:%M:%S')} =====\n".encode())
    f.flush()
    s = None
    while time.time() < end:
        try:
            if s is None or not s.is_open:
                s = serial.Serial(PORT, 115200, timeout=0.05)
                f.write(f"[{time.strftime('%H:%M:%S')}] (re)opened {PORT}\n".encode()); f.flush()
            n = s.in_waiting
            if n > 0:
                data = s.read(n)
                f.write(data); f.flush()
            else:
                time.sleep(0.05)
            now = time.time()
            if now - last_marker > MARKER_EVERY_S:
                f.write(f"\n[--TIME-- {time.strftime('%H:%M:%S')} uptime+{int(now-start)}s]\n".encode())
                f.flush()
                last_marker = now
        except (serial.SerialException, OSError) as e:
            f.write(f"\n[{time.strftime('%H:%M:%S')} SERIAL ERROR: {e}]\n".encode()); f.flush()
            try:
                if s: s.close()
            except: pass
            s = None
            time.sleep(2)
    f.write(f"\n===== OVERNIGHT CAPTURE END {time.strftime('%Y-%m-%d %H:%M:%S')} =====\n".encode())
