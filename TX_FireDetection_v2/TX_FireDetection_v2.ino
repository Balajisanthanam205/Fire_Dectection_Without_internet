// ============================================================
//  TX ESP32 — Fire Detection Sensor Node (FIXED v2)
//
//  FIXES:
//  - DHT11: added INPUT_PULLUP note + validation guard
//  - Soil pin: changed to GPIO 33 with floating-pin guard
//  - MQ2/Flame: clarified ADC inversion logic
//  - Danger thresholds tuned for real-world values
// ============================================================

#include <SPI.h>
#include <LoRa.h>
#include <DHT.h>

// ── LoRa Pins ────────────────────────────────────────────────
#define LORA_SCK    18
#define LORA_MISO   19
#define LORA_MOSI   23
#define LORA_CS      5
#define LORA_RST    14
#define LORA_DIO0    2

// ── Sensor Pins ──────────────────────────────────────────────
#define DHT_PIN      4     // DHT11 data — MUST have 10kΩ pull-up to 3.3V!
#define DHT_TYPE     DHT11
#define MQ2_PIN     34     // MQ2 AOUT — ADC1, GPIO 34 (input only, no pull-up needed)
#define FLAME_PIN   35     // Flame sensor AOUT — ADC1, GPIO 35 (input only)
#define SOIL_PIN    33     // Soil moisture AOUT — ADC1, GPIO 33

// ── Settings ─────────────────────────────────────────────────
#define TX_INTERVAL    5000   // Transmit every 5 seconds
#define SOIL_CONNECTED true   // Set false if soil sensor NOT plugged in (stops noise)
#define MQ2_WARMUP_MS  30000  // MQ2 needs 30s warmup before readings are valid

DHT dht(DHT_PIN, DHT_TYPE);

unsigned long lastTx       = 0;
unsigned long startupTime  = 0;
int           packetCount  = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  startupTime = millis();

  Serial.println("\n===== TX Fire Detection Node v2 Starting =====");
  Serial.println("[IMPORTANT] DHT11 requires a 10kΩ pull-up resistor");
  Serial.println("            between DATA pin and 3.3V — check wiring!");

  dht.begin();
  delay(2000);  // DHT11 needs 2s after power-on before first read

  // ADC-only pins — input by default, no configuration needed
  // GPIO 34 and 35 are INPUT ONLY on ESP32 — do NOT set pullup on them
  if (SOIL_CONNECTED) {
    // GPIO 33 supports INPUT (it's a normal GPIO unlike 34/35)
    pinMode(SOIL_PIN, INPUT);
  }

  Serial.println("[Sensors] DHT11=GPIO4 | MQ2=GPIO34 | Flame=GPIO35 | Soil=GPIO33");
  Serial.println("[MQ2] Warming up — readings invalid for first 30 seconds");

  // ── LoRa init ───────────────────────────────────────────────
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("[LoRa] INIT FAILED! Check wiring.");
    while (true) delay(1000);
  }

  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setTxPower(17);

  Serial.println("[LoRa] Ready at 433MHz | SF7 | BW125 | CR4/5 | 17dBm");
  Serial.println("===== Setup Complete =====\n");
}

void loop() {
  if (millis() - lastTx >= TX_INTERVAL) {
    lastTx = millis();
    readAndSend();
  }
}

void readAndSend() {
  // ── DHT11 ─────────────────────────────────────────────────
  float temperature = dht.readTemperature();
  float humidity    = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("[DHT11] READ FAILED — check 10kΩ pull-up resistor on DATA pin!");
    Serial.println("[DHT11] Sending placeholder -1 values so RX still gets a packet");
    temperature = -1;
    humidity    = -1;
  }

  // Sanity check: DHT11 range is 0–50°C, 20–90% RH
  if (temperature < 0 || temperature > 60) {
    Serial.println("[DHT11] WARN: temp " + String(temperature) +
                   "°C out of normal range — check pull-up resistor!");
  }

  // ── MQ2 Gas Sensor ────────────────────────────────────────
  // MQ2 AOUT: higher raw ADC = more gas/smoke detected
  // In clean air, MQ2 reads ~300-600 raw (7-15% after mapping)
  // With gas/smoke, it reads much higher (>2000 raw = alarm territory)
  bool mq2Warmed = (millis() - startupTime) > MQ2_WARMUP_MS;
  int  mq2Raw    = analogRead(MQ2_PIN);
  int  mq2Pct    = map(mq2Raw, 0, 4095, 0, 100);
  mq2Pct = constrain(mq2Pct, 0, 100);

  if (!mq2Warmed) {
    Serial.println("[MQ2] Still warming up — ignoring reading for danger calc");
  }

  // ── Flame Sensor ──────────────────────────────────────────
  // Most KY-026 / similar flame sensors:
  // AOUT HIGH (~4095) = no flame detected (ambient IR)
  // AOUT LOW  (~0)    = flame detected (sensor absorbs 760-1100nm IR)
  // So we INVERT: flamePct 0% = no flame, 100% = strong flame
  int flameRaw = analogRead(FLAME_PIN);
  int flamePct = map(flameRaw, 4095, 0, 0, 100);  // Inverted!
  flamePct = constrain(flamePct, 0, 100);

  // ── Soil Moisture ─────────────────────────────────────────
  // If sensor NOT connected, pin floats — send -1 to indicate no sensor
  int soilPct = -1;
  if (SOIL_CONNECTED) {
    int soilRaw = analogRead(SOIL_PIN);
    // Most soil sensors: 4095 = dry (in air), ~1000-1500 = saturated
    soilPct = map(soilRaw, 4095, 800, 0, 100);
    soilPct = constrain(soilPct, 0, 100);
  }

  // ── Danger Level ──────────────────────────────────────────
  int dangerLevel = 0;
  if (temperature > 0) {  // Only compute if DHT11 is working
    dangerLevel = computeDangerLevel(
      temperature, humidity,
      mq2Warmed ? mq2Pct : 0,  // Ignore MQ2 during warmup
      flamePct
    );
  }

  // ── Build + Send CSV packet ───────────────────────────────
  // Format: temp,hum,mq2,flame,soil,danger,packetId
  packetCount++;
  String packet = String(temperature, 1) + "," +
                  String(humidity, 1)    + "," +
                  String(mq2Pct)         + "," +
                  String(flamePct)       + "," +
                  String(soilPct)        + "," +
                  String(dangerLevel)    + "," +
                  String(packetCount);

  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();

  // ── Serial Monitor ────────────────────────────────────────
  Serial.println("─────────────────────────────");
  Serial.println("[TX #" + String(packetCount) + "]");
  Serial.println("  Temp      : " + String(temperature, 1) + " °C" +
                 (temperature < 5 ? " ← WRONG! Fix DHT11 pull-up" : ""));
  Serial.println("  Humidity  : " + String(humidity, 1) + " %");
  Serial.println("  MQ2 Gas   : " + String(mq2Pct) + "%" +
                 (mq2Warmed ? "" : " (warming up)"));
  Serial.println("  Flame     : " + String(flamePct) + "%");
  Serial.println("  Soil      : " + (soilPct == -1 ? "Not connected" : String(soilPct) + "%"));
  Serial.println("  Danger    : " + String(dangerLevel) + " / 3");
  Serial.println("  Payload   : " + packet);
}

// ─────────────────────────────────────────────────────────────
//  Danger level — calibrated for real sensor values
//
//  LEVEL 0 (GREEN)  — Normal room conditions, all clear
//  LEVEL 1 (YELLOW) — One sensor slightly elevated, investigate
//  LEVEL 2 (RED)    — Multiple sensors elevated or one very high
//  LEVEL 3 (ALARM)  — Fire condition confirmed
// ─────────────────────────────────────────────────────────────
int computeDangerLevel(float temp, float hum, int mq2, int flame) {
  int score = 0;

  // ── Temperature ───────────────────────────────────────────
  // Normal room: 15-35°C. Fire nearby causes rapid rise.
  if      (temp >= 70)  score += 4;  // Definite fire nearby
  else if (temp >= 55)  score += 3;
  else if (temp >= 45)  score += 2;
  else if (temp >= 38)  score += 1;  // Slightly warm, could be summer

  // ── Flame Sensor ──────────────────────────────────────────
  // This is the MOST RELIABLE fire indicator
  // 0% = no flame, 100% = flame detected
  // Even a small candle in the room reads ~30-50%
  if      (flame >= 60)  score += 5;  // Strong open flame
  else if (flame >= 35)  score += 3;  // Clear flame detected
  else if (flame >= 15)  score += 1;  // Possible flame / bright IR source

  // ── MQ2 Gas / Smoke ───────────────────────────────────────
  // Clean air baseline: ~5-20% after warmup
  // Light smoke: ~25-45%
  // Heavy smoke/gas: >50%
  if      (mq2 >= 60)  score += 4;
  else if (mq2 >= 40)  score += 2;
  else if (mq2 >= 25)  score += 1;

  // ── Humidity drop (fire dries the air) ────────────────────
  // Only meaningful if temp is also high
  if (temp >= 40 && hum < 20) score += 1;

  // ── Map score to danger level ──────────────────────────────
  //  Score 0-1  → Level 0 (Safe)
  //  Score 2-3  → Level 1 (Caution — yellow)
  //  Score 4-6  → Level 2 (Warning — red)
  //  Score 7+   → Level 3 (Fire! — all + buzzer)
  if      (score >= 7)  return 3;
  else if (score >= 4)  return 2;
  else if (score >= 2)  return 1;
  return 0;
}
