import serial
import time
import sys

try:
    s = serial.Serial('COM6', 115200, timeout=0.1)
    t0 = time.time()
    with open('uart_log.txt', 'w') as f:
        while time.time() - t0 < 20:
            data = s.read(s.in_waiting or 1)
            if data:
                text = data.decode('utf-8', 'replace')
                print(text, end='')
                f.write(text)
    s.close()
except Exception as e:
    print(f"Error: {e}")
