"""Overnight serial capture for unified_cam.

Reads COM8 forever. Writes raw bytes to mon_overnight.txt (append).
Emits filtered single-line events to stdout for the Monitor tool to
notify on (resets, panics, errors, IR/dispense events).

Run via the Monitor tool with persistent=true.
"""
import serial, time, re, sys, datetime

LOG_PATH = "mon_overnight.txt"
PORT = "COM8"
BAUD = 115200

# Patterns that should produce a notification.
# Tightened 02:23 — excluded routine recover_bus / camera attempt lines
# that flood the channel during normal boot. Now only fault, panic,
# init-complete, and dispense events trigger.
CRIT = re.compile(
    r"ESP-ROM:|"
    r"rst:0x[0-9a-fA-F]+ \(|"
    r"Boot #|"
    r"PANIC|"
    r"Guru Meditation|"
    r"abort\(\)|"
    r"assert |"
    r"Backtrace|"
    r"Saved PC:|"
    r"brownout|"
    r"Stack canary|"
    r"TWDT|"
    r"Task watchdog|"
    r"Hardware I2C failure|"
    r"pca9685.*failed|"
    r"mutex busy|"
    r"deadlock|"
    r"corrupt|"
    r"Failed to create|"
    r"app_main: init complete|"
    r"Drop \d+/\d+|"
    r"DETECTED pill|"
    r"MISSED|"
    r"servo WORK med"
)

# Lines matching these regexes are NEVER emitted (suppression list).
SUPPRESS = re.compile(
    r"recover_bus: bus re-initialized|"
    r"I2C runtime recovery|"
    r"tearing down bus|"
    r"GPIO\[\d+\]\| InputEn|"
    r"sccb_i2c:|"
    r"ov5647:|"
    r"Camera attempt \d+ failed"
)

# ANSI strip
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def open_serial():
    """Open the serial port, retrying every 10 s if another process
    is holding it. Returns the Serial object once opened."""
    while True:
        try:
            return serial.Serial(PORT, BAUD, timeout=0.2)
        except Exception as e:
            ts = datetime.datetime.now().strftime("%H:%M:%S")
            print(f"{ts} PORT-WAIT: {e}", flush=True)
            time.sleep(10)


def main():
    log = open(LOG_PATH, "ab", buffering=0)
    line_buf = ""
    print(f"START {datetime.datetime.now().isoformat()} on {PORT}", flush=True)
    s = open_serial()
    consecutive_errors = 0
    while True:
        try:
            d = s.read(s.in_waiting or 1)
            consecutive_errors = 0
            if not d:
                continue
            log.write(d)
            text = d.decode("utf-8", "replace")
            line_buf += text
            while "\n" in line_buf:
                line, line_buf = line_buf.split("\n", 1)
                clean = ANSI.sub("", line).rstrip("\r")
                if SUPPRESS.search(clean):
                    continue
                if CRIT.search(clean):
                    ts = datetime.datetime.now().strftime("%H:%M:%S")
                    snippet = clean[:200]
                    print(f"{ts} {snippet}", flush=True)
        except Exception as e:
            consecutive_errors += 1
            if consecutive_errors == 1:
                ts = datetime.datetime.now().strftime("%H:%M:%S")
                print(f"{ts} EXC: {e} — closing port + waiting to reopen", flush=True)
            # Close the dead handle and wait for the port to be free.
            try:
                s.close()
            except Exception:
                pass
            time.sleep(5)
            s = open_serial()
            ts = datetime.datetime.now().strftime("%H:%M:%S")
            print(f"{ts} REOPENED {PORT}", flush=True)
            consecutive_errors = 0


if __name__ == "__main__":
    main()
