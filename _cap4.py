import serial, time
s = serial.Serial("COM6", 115200, timeout=0.5)
# Normal-mode reset: keep DTR high (no download strap), pulse RTS
s.setDTR(False)
s.setRTS(True)   # reset asserted
time.sleep(0.1)
s.setRTS(False)  # release reset, runs firmware
print("Reset issued (run mode); capturing 45s...", flush=True)
end = time.time() + 45
total = 0
with open(r"D:\project\ddddddddd\unified_cam\monitor_vl53_r4.txt", "wb") as f:
    while time.time() < end:
        d = s.read(4096)
        if d:
            f.write(d); f.flush()
            total += len(d)
print(f"done, {total} bytes")
