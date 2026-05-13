import serial, time
s = serial.Serial("COM6", 115200, timeout=0.5)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
print("Reset issued, capturing 120s...", flush=True)
end = time.time() + 120
total = 0
with open(r"D:\project\ddddddddd\unified_cam\monitor_long.txt", "wb") as f:
    while time.time() < end:
        d = s.read(4096)
        if d:
            f.write(d); f.flush()
            total += len(d)
print(f"done, {total} bytes")
