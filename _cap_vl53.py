import serial, time, sys
port = "COM6"
baud = 115200
duration = 60
out = "monitor_vl53_after.log"
s = serial.Serial(port, baud, timeout=0.2)
# trigger reset via DTR/RTS toggle
s.setDTR(False); s.setRTS(True); time.sleep(0.1)
s.setDTR(False); s.setRTS(False); time.sleep(0.05)
t_end = time.time() + duration
with open(out, "wb") as f:
    while time.time() < t_end:
        data = s.read(4096)
        if data:
            f.write(data); f.flush()
s.close()
print("captured")
