import serial, sys, time
port = sys.argv[1] if len(sys.argv) > 1 else 'COM6'
baud = 115200
out = sys.argv[2] if len(sys.argv) > 2 else 'monitor_reset_watch.txt'
duration = int(sys.argv[3]) if len(sys.argv) > 3 else 240
ser = serial.Serial(port, baud, timeout=1)
print(f"opened {port} @ {baud}, capturing to {out} for {duration}s", flush=True)
ser.setDTR(False); ser.setRTS(True); time.sleep(0.1)
ser.setDTR(False); ser.setRTS(False); time.sleep(0.1)
print("[reset pulse sent via RTS]", flush=True)
start = time.time()
with open(out, 'wb') as f:
    while time.time() - start < duration:
        data = ser.read(4096)
        if data:
            f.write(data); f.flush()
            try:
                sys.stdout.write(data.decode('utf-8', errors='replace'))
                sys.stdout.flush()
            except Exception:
                pass
ser.close()
print("\n[capture done]", flush=True)
