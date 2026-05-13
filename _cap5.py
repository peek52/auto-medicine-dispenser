import serial, time
s = serial.Serial("COM6", 115200, timeout=0.05)
print("LIVE capture 90s (no reset)...", flush=True)
end = time.time() + 90
total = 0
last_print = time.time()
with open(r"D:\project\ddddddddd\unified_cam\mon_live2.txt", "wb") as f:
    while time.time() < end:
        d = s.read(8192)
        if d:
            f.write(d); f.flush()
            total += len(d)
            if time.time() - last_print > 10:
                print(f"  {total} bytes", flush=True)
                last_print = time.time()
print(f"done, {total} bytes total")
