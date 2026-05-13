import serial, time
s = serial.Serial("COM6", 115200, timeout=0.02)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
print("Reset, capturing 100s with in_waiting drain...", flush=True)
end = time.time() + 100
total = 0
last_print = time.time()
with open(r"D:\project\ddddddddd\unified_cam\mon_full.txt", "wb") as f:
    while time.time() < end:
        n = s.in_waiting
        if n > 0:
            d = s.read(n)
            f.write(d); f.flush()
            total += len(d)
        else:
            time.sleep(0.02)
        if time.time() - last_print > 8:
            print(f"  {total} bytes", flush=True)
            last_print = time.time()
print(f"done, {total} bytes")
