import serial, time
s = serial.Serial("COM6", 115200, timeout=0.02)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
print("Reset+capture 180s...", flush=True)
end = time.time() + 180
total = 0
last_print = time.time()
last_data = time.time()
with open(r"D:\project\ddddddddd\unified_cam\mon_full2.txt", "wb") as f:
    while time.time() < end:
        n = s.in_waiting
        if n > 0:
            d = s.read(n); f.write(d); f.flush()
            total += len(d); last_data = time.time()
        else:
            time.sleep(0.02)
        if time.time() - last_print > 15:
            silent = int(time.time() - last_data)
            print(f"  {total}B (silent {silent}s)", flush=True)
            last_print = time.time()
print(f"done, {total} bytes")
