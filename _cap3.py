import serial, time
s = serial.Serial("COM6", 115200, timeout=0.5)
# Toggle DTR/RTS to reset the board
s.setDTR(False); s.setRTS(True); time.sleep(0.1)
s.setDTR(True); s.setRTS(False); time.sleep(0.1)
s.setRTS(False)
print("Reset issued; capturing 45s...", flush=True)
end = time.time() + 45
total = 0
with open(r"D:\project\ddddddddd\unified_cam\monitor_vl53_r3.txt", "wb") as f:
    while time.time() < end:
        d = s.read(4096)
        if d:
            f.write(d); f.flush()
            total += len(d)
print(f"done, captured {total} bytes")
