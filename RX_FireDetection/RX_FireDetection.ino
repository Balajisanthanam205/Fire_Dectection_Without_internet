// ============================================================
//  RX ESP32 — Fire Detection Receiver Node
//  - Receives LoRa packets from TX node
//  - Drives LEDs and buzzer based on danger level
//  - Pushes data to Supabase via HTTP POST (needs WiFi)
// ============================================================

#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── LoRa Pins (same as your tested config) ──────────────────
#define LORA_SCK    18
#define LORA_MISO   19
#define LORA_MOSI   23
#define LORA_CS      5
#define LORA_RST    14
#define LORA_DIO0    2

// ── Output Pins ──────────────────────────────────────────────
#define LED_GREEN   25
#define LED_YELLOW  26
#define LED_RED     27
#define BUZZER_PIN  32

// ── WiFi Credentials ─────────────────────────────────────────
const char* WIFI_SSID     = "S Balaji";
const char* WIFI_PASSWORD = "2127230701019";

// ── Supabase Config ──────────────────────────────────────────
// Replace with your actual project values from Supabase dashboard
const char* SUPABASE_URL    = "https://xpvxfwxyhfsxvxbwhxul.supabase.co/rest/v1/sensor_readings";
const char* SUPABASE_APIKEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inhwdnhmd3h5aGZzeHZ4YndoeHVsIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzQ1MzI4NzIsImV4cCI6MjA5MDEwODg3Mn0.7zTbeNlPgoHVHo7sNCeeDv031-GGdRlBB-Y3HD9xXNs";

// ── State ─────────────────────────────────────────────────────
bool wifiConnected   = false;
int  lastDangerLevel = -1;   // Track changes to avoid redundant LED updates

// Buzzer pattern timing
unsigned long buzzerLastToggle = 0;
bool          buzzerState      = false;
int           buzzerBeepCount  = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n===== RX Fire Detection Receiver Starting =====");

  // ── Output pin setup ────────────────────────────────────────
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  // All off at start
  allLedsOff();
  digitalWrite(BUZZER_PIN, LOW);
  Serial.println("[Pins] LEDs: G=25 Y=26 R=27 | Buzzer=32");

  // ── WiFi (non-blocking connect) ────────────────────────────
  Serial.println("[WiFi] Connecting to " + String(WIFI_SSID) + "...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());
  } else {
    wifiConnected = false;
    Serial.println("\n[WiFi] NOT connected — will still alert locally via LEDs/buzzer");
    Serial.println("[WiFi] Data will NOT be pushed to Supabase until WiFi is available");
  }

  // ── LoRa init ───────────────────────────────────────────────
  Serial.println("[LoRa] Initializing...");
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("[LoRa] INIT FAILED! Check wiring.");
    while (true) {
      delay(1000);
      Serial.println("[LoRa] Stuck — please reset.");
    }
  }

  // Must match TX exactly!
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);

  Serial.println("[LoRa] Ready — listening at 433MHz | SF7 | BW125 | CR4/5");

  // Startup LED test: flash all three once
  startupBlink();

  Serial.println("===== Setup Complete — Waiting for packets =====\n");
}

void loop() {
  // Check for incoming LoRa packet
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    handlePacket();
  }

  // WiFi reconnect watchdog
  if (millis() % 30000 < 100 && WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Reconnecting...");
    WiFi.reconnect();
    delay(5000);
    wifiConnected = (WiFi.status() == WL_CONNECTED);
  }
}

// ─────────────────────────────────────────────────────────────
//  Handle incoming LoRa packet
// ─────────────────────────────────────────────────────────────
void handlePacket() {
  String raw = "";
  while (LoRa.available()) {
    raw += (char)LoRa.read();
  }
  int rssi = LoRa.packetRssi();

  Serial.println("─────────────────────────────");
  Serial.println("[LoRa] Packet received: \"" + raw + "\"");
  Serial.println("[LoRa] RSSI: " + String(rssi) + " dBm");

  // ── Parse CSV: temp,hum,mq2,flame,soil,danger,packetId ────
  float temp    = 0, hum = 0;
  int   mq2     = 0, flame = 0, soil = 0, danger = 0, pktId = 0;

  int parsed = sscanf(raw.c_str(), "%f,%f,%d,%d,%d,%d,%d",
                      &temp, &hum, &mq2, &flame, &soil, &danger, &pktId);

  if (parsed != 7) {
    Serial.println("[Parse] ERROR — expected 7 fields, got " + String(parsed));
    Serial.println("[Parse] Raw: " + raw);
    return;
  }

  Serial.println("[Data] Pkt#" + String(pktId) +
                 " | Temp=" + String(temp, 1) + "°C" +
                 " | Hum=" + String(hum, 1) + "%" +
                 " | Gas=" + String(mq2) + "%" +
                 " | Flame=" + String(flame) + "%" +
                 " | Soil=" + String(soil) + "%" +
                 " | Danger=" + String(danger));

  // ── Drive LEDs and Buzzer ─────────────────────────────────
  setAlerts(danger);

  // ── Push to Supabase ──────────────────────────────────────
  if (wifiConnected) {
    pushToSupabase(temp, hum, mq2, flame, soil, danger, rssi, pktId);
  } else {
    Serial.println("[Supabase] Skipped — no WiFi");
  }
}

// ─────────────────────────────────────────────────────────────
//  LED + Buzzer control based on danger level
// ─────────────────────────────────────────────────────────────
void setAlerts(int level) {
  allLedsOff();
  digitalWrite(BUZZER_PIN, LOW);

  switch (level) {
    case 0:  // SAFE — green only, silent
      digitalWrite(LED_GREEN, HIGH);
      Serial.println("[Alert] Level 0 — SAFE — Green LED ON");
      break;

    case 1:  // CAUTION — yellow, silent
      digitalWrite(LED_YELLOW, HIGH);
      Serial.println("[Alert] Level 1 — CAUTION — Yellow LED ON");
      break;

    case 2:  // WARNING — red + slow beep
      digitalWrite(LED_RED, HIGH);
      Serial.println("[Alert] Level 2 — WARNING — Red LED ON + beep");
      beepBuzzer(3, 200, 400);  // 3 beeps, 200ms on, 400ms off
      break;

    case 3:  // FIRE — all LEDs + fast continuous beep
      digitalWrite(LED_GREEN,  HIGH);
      digitalWrite(LED_YELLOW, HIGH);
      digitalWrite(LED_RED,    HIGH);
      Serial.println("[Alert] Level 3 — FIRE! — All LEDs ON + alarm");
      beepBuzzer(6, 100, 100);  // Fast rapid beeping
      break;

    default:
      Serial.println("[Alert] Unknown danger level: " + String(level));
      break;
  }

  lastDangerLevel = level;
}

// Blocking buzzer beep pattern
void beepBuzzer(int count, int onMs, int offMs) {
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < count - 1) delay(offMs);
  }
}

void allLedsOff() {
  digitalWrite(LED_GREEN,  LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_RED,    LOW);
}

void startupBlink() {
  Serial.println("[Test] Startup LED test...");
  for (int i = 0; i < 2; i++) {
    digitalWrite(LED_GREEN,  HIGH); delay(200);
    digitalWrite(LED_GREEN,  LOW);
    digitalWrite(LED_YELLOW, HIGH); delay(200);
    digitalWrite(LED_YELLOW, LOW);
    digitalWrite(LED_RED,    HIGH); delay(200);
    digitalWrite(LED_RED,    LOW);
  }
  // Quick buzzer chirp
  digitalWrite(BUZZER_PIN, HIGH); delay(80);
  digitalWrite(BUZZER_PIN, LOW);
  Serial.println("[Test] Done — all hardware OK");
}

// ─────────────────────────────────────────────────────────────
//  Push sensor reading to Supabase via HTTP POST
// ─────────────────────────────────────────────────────────────
void pushToSupabase(float temp, float hum, int mq2, int flame,
                    int soil, int danger, int rssi, int pktId) {
  HTTPClient http;
  http.begin(SUPABASE_URL);
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("apikey",        SUPABASE_APIKEY);
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_APIKEY));
  http.addHeader("Prefer",        "return=minimal");  // Don't return inserted row

  // Build JSON payload using ArduinoJson
  StaticJsonDocument<256> doc;
  doc["temperature"]   = serialized(String(temp, 1));
  doc["humidity"]      = serialized(String(hum, 1));
  doc["gas_level"]     = mq2;
  doc["flame_level"]   = flame;
  doc["soil_moisture"] = soil;
  doc["danger_level"]  = danger;
  doc["rssi"]          = rssi;
  doc["packet_id"]     = pktId;

  String body;
  serializeJson(doc, body);

  Serial.println("[Supabase] POST → " + body);

  int httpCode = http.POST(body);

  if (httpCode == 201) {
    Serial.println("[Supabase] Saved OK (HTTP 201)");
  } else if (httpCode > 0) {
    Serial.println("[Supabase] HTTP " + String(httpCode) + " — " + http.getString());
  } else {
    Serial.println("[Supabase] Connection failed: " + http.errorToString(httpCode));
    wifiConnected = false;  // Mark for reconnect attempt
  }

  http.end();
}
