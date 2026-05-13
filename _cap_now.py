import serial, time
s = serial.Serial("COM6", 115200, timeout=0.01)
print("LIVE 120s slow drain (no reset)...", flush=True)
end = time.time() + 120
total = 0
last_print = time.time()
last_data = time.time()
with open(r"D:\project\ddddddddd\unified_cam\mon_now.txt", "wb") as f:
    while time.time() < end:
        d = s.read(8192)
        if d:
            f.write(d); f.flush()
            total += len(d); last_data = time.time()
        if time.time() - last_print > 10:
            silent = int(time.time() - last_data)
            print(f"  {total}B silent={silent}s", flush=True); last_print = time.time()
print(f"done {total}B")
