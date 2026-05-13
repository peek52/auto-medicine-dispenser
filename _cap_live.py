import serial, time
s = serial.Serial("COM6", 115200, timeout=0.5)
print("Capturing 60s of live monitor (no reset)...", flush=True)
end = time.time() + 60
total = 0
with open(r"D:\project\ddddddddd\unified_cam\monitor_live.txt", "wb") as f:
    while time.time() < end:
        d = s.read(4096)
        if d:
            f.write(d); f.flush()
            total += len(d)
print(f"done, {total} bytes")
