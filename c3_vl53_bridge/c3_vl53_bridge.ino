/*
 * ESP32-C3 Super Mini — VL53L0X × 6 via TCA9548A → UART CSV bridge
 *
 * Reads six VL53L0X time-of-flight sensors sitting behind a TCA9548A
 * I2C multiplexer and streams the distance readings as CSV out of
 * Serial1 to an external host (ESP32-P4-Nano in this project).
 *
 * Hardware:
 *   ESP32-C3 Super Mini
 *   ├── I2C: SDA=GPIO6, SCL=GPIO7  ────► TCA9548A ─┬─► CH2: VL53L0X #0
 *   │                                              ├─► CH3: VL53L0X #1
 *   │                                              ├─► CH4: VL53L0X #2
 *   │                                              ├─► CH5: VL53L0X #3
 *   │                                              ├─► CH6: VL53L0X #4
 *   │                                              └─► CH7: VL53L0X #5
 *   ├── UART1 TX: GPIO4 ───► P4 RX (GPIO14)
 *   └── UART1 RX: GPIO5 ◄─── P4 TX (GPIO15)   (currently unused, reserved)
 *
 *   GND must be common between the C3 and the P4.
 *
 * Wire protocol (one line every loop, ~10 Hz):
 *   "vl53:<s0>,<s1>,<s2>,<s3>,<s4>,<s5>\n"
 *
 *   sN = distance in millimetres (integer) for the sensor on TCA channel
 *        N+2. -1 if the sensor failed to init OR the reading was out of
 *        range / timed out. The "vl53:" prefix is optional on the P4 side
 *        but useful for grep when watching the C3's USB-CDC debug echo.
 *
 * Libraries required (install via Library Manager):
 *   - "Adafruit VL53L0X" by Adafruit (also pulls in Adafruit BusIO)
 *
 * Board: "ESP32C3 Dev Module" (or "ESP32 Family Device") with USB CDC On Boot enabled
 *   so Serial.* still works over the USB port for debug output.
 */

#include <Wire.h>
#include "Adafruit_VL53L0X.h"

// ── User-tunable pin map ──────────────────────────────────────────
#define SDA_PIN       6
#define SCL_PIN       7
#define UART_TX_PIN   4         // → P4 GPIO14 (RX)
#define UART_RX_PIN   5         // ← P4 GPIO15 (TX, currently unused)
#define UART_BAUD     115200

// ── Sensor map ────────────────────────────────────────────────────
#define TCA_ADDR      0x70
#define NUM_SENSORS   6
// VL53 sit on TCA channels 2..7 (skipping ch0/ch1 which are wired to
// other peripherals on this carrier). Index in the CSV output:
//   csv[i] ← TCA_CHANNELS[i] sensor reading
static const uint8_t TCA_CHANNELS[NUM_SENSORS] = { 2, 3, 4, 5, 6, 7 };

// Loop period for the CSV output. ~100 ms = 10 Hz update is plenty
// for the P4's /vl53 dashboard which polls at 500 ms.
#define LOOP_PERIOD_MS  100

// ── State ─────────────────────────────────────────────────────────
Adafruit_VL53L0X lox[NUM_SENSORS];
bool             sensor_ok[NUM_SENSORS] = { false };

// ── Helpers ───────────────────────────────────────────────────────

// Select exactly ONE downstream channel on the TCA9548A. Writing 0
// disables all channels — useful between sensors if you suspect bus
// contention, but slows things down so the routine here selects one
// at a time and trusts the multiplexer.
static void tcaSelect(uint8_t ch) {
  if (ch > 7) return;
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << ch);
  Wire.endTransmission();
}

// One-shot range measurement on the currently-selected channel.
// Returns mm distance, or -1 on out-of-range / timeout. Pass the
// per-sensor Adafruit driver instance.
static int readDistanceMm(Adafruit_VL53L0X &sensor) {
  VL53L0X_RangingMeasurementData_t m;
  sensor.rangingTest(&m, false);
  // Status 4 = "out of range" per Adafruit driver convention.
  // Anything > 8000 mm is meaningless for VL53L0X (max ~2 m) and
  // usually indicates a glitch; clamp those to -1 too.
  if (m.RangeStatus == 4)                  return -1;
  if (m.RangeMilliMeter > 8000)            return -1;
  return (int)m.RangeMilliMeter;
}

// ── setup() ───────────────────────────────────────────────────────
void setup() {
  // USB CDC — debug echo. Enabled at compile time via "USB CDC On Boot".
  Serial.begin(115200);
  delay(300);    // brief settle so the boot banner doesn't race the host
  Serial.println();
  Serial.println("ESP32-C3 VL53 bridge — starting");

  // Hardware UART1 going to the P4. RX is reserved for a future command
  // channel; only TX is actively used today.
  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  // I2C bus for the TCA9548A. 100 kHz is conservative and works well
  // with daisy-chained VL53L0X — push to 400 kHz only after confirming
  // all six sensors are stable.
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  // Probe + initialise each sensor. The TCA9548A makes all six look
  // like the same 0x29 address — we just switch channels between
  // each begin() call so the driver talks to one sensor at a time.
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    tcaSelect(TCA_CHANNELS[i]);
    delay(10);
    if (lox[i].begin(0x29, false, &Wire)) {
      sensor_ok[i] = true;
      Serial.print("  Sensor "); Serial.print(i);
      Serial.print(" (TCA ch "); Serial.print(TCA_CHANNELS[i]);
      Serial.println("): OK");
    } else {
      sensor_ok[i] = false;
      Serial.print("  Sensor "); Serial.print(i);
      Serial.print(" (TCA ch "); Serial.print(TCA_CHANNELS[i]);
      Serial.println("): FAIL");
    }
    delay(20);
  }

  Serial.println("Bridge ready — streaming CSV to UART1 @ 115200");
}

// ── loop() ────────────────────────────────────────────────────────
void loop() {
  int dist[NUM_SENSORS];

  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    if (!sensor_ok[i]) {
      dist[i] = -1;
      continue;
    }
    tcaSelect(TCA_CHANNELS[i]);
    dist[i] = readDistanceMm(lox[i]);
  }

  // Emit one CSV line. The P4 listener accepts an optional "vl53:"
  // prefix; we include it so the same line is grep-friendly when
  // monitoring the C3 over USB-CDC.
  char buf[96];
  int n = snprintf(buf, sizeof(buf), "vl53:%d,%d,%d,%d,%d,%d\r\n",
                   dist[0], dist[1], dist[2], dist[3], dist[4], dist[5]);
  if (n > 0) {
    Serial1.write((const uint8_t*)buf, n);   // → P4
    Serial.write((const uint8_t*)buf, n);    // ↻ USB CDC echo for debug
  }

  delay(LOOP_PERIOD_MS);
}
