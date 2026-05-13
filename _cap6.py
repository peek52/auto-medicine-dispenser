import serial, time
s = serial.Serial("COM6", 115200, timeout=0.05)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
print("Reset issued, capturing 60s...", flush=True)
end = time.time() + 60
total = 0
last_print = time.time()
with open(r"D:\project\ddddddddd\unified_cam\mon_reset.txt", "wb") as f:
    while time.time() < end:
        d = s.read(8192)
        if d:
            f.write(d); f.flush()
            total += len(d)
            if time.time() - last_print > 5:
                print(f"  {total} bytes @ {int(time.time()-end+60)}s", flush=True)
                last_print = time.time()
print(f"done, {total} bytes")
