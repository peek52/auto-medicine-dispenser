import serial, time
s = serial.Serial("COM6", 115200, timeout=0.02)
print("LIVE 240s (no reset, while you test)...", flush=True)
end = time.time() + 240
total = 0
last_print = time.time()
last_data = time.time()
last_mark = time.time()
with open(r"D:\project\ddddddddd\unified_cam\mon_test.txt", "wb") as f:
    f.write(b"\n===== LIVE TEST CAPTURE START =====\n")
    while time.time() < end:
        n = s.in_waiting
        if n > 0:
            d = s.read(n); f.write(d); f.flush()
            total += len(d); last_data = time.time()
        else:
            time.sleep(0.02)
        now = time.time()
        if now - last_mark > 30:
            f.write(f"\n[--MARK-- t={int(now-end+240)}s total={total}B]\n".encode())
            f.flush()
            last_mark = now
        if now - last_print > 15:
            silent = int(now - last_data)
            print(f"  {total}B (silent {silent}s)", flush=True)
            last_print = now
print(f"done, {total} bytes")
