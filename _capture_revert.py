import serial, time, sys
s = serial.Serial("COM6", 115200, timeout=0.2)
end = time.time() + 35
with open(r"D:\project\ddddddddd\unified_cam\monitor_vl53_revert.txt", "wb") as f:
    while time.time() < end:
        d = s.read(4096)
        if d:
            f.write(d); f.flush()
print("done")
