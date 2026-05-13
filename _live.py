import serial, time
s = serial.Serial("COM6", 115200, timeout=0.02)
print("LIVE 60s (no reset)...", flush=True)
end = time.time() + 60
total = 0
last = time.time()
with open(r"D:\project\ddddddddd\unified_cam\mon_freeze.txt", "wb") as f:
    while time.time() < end:
        n = s.in_waiting
        if n > 0:
            d = s.read(n); f.write(d); f.flush(); total += len(d)
        else:
            time.sleep(0.05)
        if time.time() - last > 8:
            print(f"  {total} bytes", flush=True); last = time.time()
print(f"done {total} bytes")
