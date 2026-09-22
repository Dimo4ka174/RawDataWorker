// RawDataWorker — Arduino current-transformer data collector.
// Publishes machine state (working / idle) to AWM over HTTP.
//
// Board:   Arduino Mega 2560 + Ethernet Shield (W5100)
// Sensor:  SCT-013 current transformer on analog pin A0
// Library: EmonLib

#include <SPI.h>
#include <Ethernet.h>
#include <EmonLib.h>
#include <avr/wdt.h>

// ============================ CONFIG ============================

// --- Network ---
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress localIp(192, 168, 10, 177);
IPAddress gateway(192, 168, 10, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);

IPAddress serverIp(192, 168, 10, 200);
const int   serverPort = 5050;
const char* SERVER_ENDPOINT = "/api/Wincc/updateMachineStatus";

// --- Machine identity ---
const char* MACHINE_NAME = "Danobat";

// --- Current transformer ---
const uint8_t CT_ANALOG_PIN  = 0;
const float   CT_CALIBRATION = 4.0f;    // EmonLib calibration coefficient
const int     IRMS_SAMPLES   = 1480;    // Samples per measurement

// --- Thresholds ---
const float MAINS_VOLTAGE_V      = 230.0f; // Nominal voltage for P ≈ U·I
const float CURRENT_THRESHOLD_A  = 0.05f;  // Above → "working"

// --- Timing ---
const unsigned long MEASURE_INTERVAL_MS = 1000;
const unsigned long HTTP_TIMEOUT_MS     = 2000;
const int           NEED_STABLE_READINGS = 3;

// ============================ STATE ============================

EthernetClient client;
EnergyMonitor  emon;

int  stableCount      = 0;
bool lastStableStatus = false;
bool lastSentStatus   = false;
unsigned long lastMeasureTime = 0;

// ============================ HTTP =============================

// Returns true if server replied 200 OK.
bool sendStatus(bool isWorking, float current, float power) {
  char body[160];
  int bodyLen = snprintf(
      body, sizeof(body),
      "{\"machineName\":\"%s\",\"Condition\":%s,\"current\":%.2f,\"power\":%.0f}",
      MACHINE_NAME,
      isWorking ? "true" : "false",
      current, power);

  if (bodyLen <= 0 || bodyLen >= (int)sizeof(body)) {
    Serial.println(F("ERR: payload overflow"));
    return false;
  }

  if (!client.connect(serverIp, serverPort)) {
    Serial.println(F("ERR: connect failed"));
    return false;
  }

  client.print(F("POST "));
  client.print(SERVER_ENDPOINT);
  client.print(F(" HTTP/1.1\r\nHost: "));
  client.print(serverIp);
  client.print(F("\r\nContent-Type: application/json\r\nContent-Length: "));
  client.print(bodyLen);
  client.print(F("\r\nConnection: close\r\n\r\n"));
  client.write((const uint8_t*)body, bodyLen);

  unsigned long deadline = millis() + HTTP_TIMEOUT_MS;
  bool httpOk   = false;
  bool changed  = false;
  char line[128];
  size_t pos = 0;

  while (millis() < deadline) {
    while (client.available()) {
      char c = client.read();
      if (c == '\n') {
        line[pos] = '\0';
        if (strstr(line, "200 OK"))            httpOk  = true;
        if (strstr(line, "\"changed\":true"))  changed = true;
        pos = 0;
      } else if (c != '\r' && pos < sizeof(line) - 1) {
        line[pos++] = c;
      }
    }
    if (!client.connected() && !client.available()) break;
  }

  client.stop();

  if (!httpOk) {
    Serial.println(F("ERR: no 200 OK"));
    return false;
  }
  Serial.print(changed ? F(" [CHANGED]") : F(" [UNCHANGED]"));
  return true;
}

// ============================ SETUP ============================

void setup() {
  wdt_disable();          // avoid reset loop during init
  Serial.begin(9600);
  delay(1000);

  Serial.println(F("========================================"));
  Serial.println(F("===     RAW DATA WORKER (current)    ==="));
  Serial.println(F("========================================"));

  Serial.print(F("Sensor... "));
  emon.current(CT_ANALOG_PIN, CT_CALIBRATION);
  Serial.println(F("OK"));

  Serial.print(F("Ethernet... "));
  Ethernet.begin(mac, localIp, dns, gateway, subnet);
  delay(1500);

  if (Ethernet.localIP() == IPAddress(0, 0, 0, 0)) {
    Serial.println(F("ERROR"));
    while (true) { delay(1000); }
  }
  Serial.print(F("OK, IP="));
  Serial.println(Ethernet.localIP());
  Serial.print(F("Server: "));
  Serial.print(serverIp);
  Serial.print(':');
  Serial.println(serverPort);
  Serial.println(F("========================================"));

  lastMeasureTime = millis();
  wdt_enable(WDTO_8S);
}

// ============================ LOOP =============================

void loop() {
  wdt_reset();
  unsigned long now = millis();

  if (now - lastMeasureTime < MEASURE_INTERVAL_MS) {
    delay(10);
    return;
  }
  lastMeasureTime = now;

  float current  = emon.calcIrms(IRMS_SAMPLES);
  float power    = current * MAINS_VOLTAGE_V;
  bool  isActive = (current > CURRENT_THRESHOLD_A);

  Serial.print(current, 2);
  Serial.print(F(" A | "));
  Serial.print(isActive ? F("Work") : F("Stop"));

  if (isActive == lastStableStatus) {
    stableCount++;

    int shown = stableCount > NEED_STABLE_READINGS
                    ? NEED_STABLE_READINGS
                    : stableCount;
    Serial.print(' ');
    Serial.print(shown);
    Serial.print('/');
    Serial.print(NEED_STABLE_READINGS);

    if (stableCount == NEED_STABLE_READINGS && isActive != lastSentStatus) {
      Serial.print(F(" -> sending..."));
      if (sendStatus(isActive, current, power)) {
        Serial.print(F(" OK"));
        lastSentStatus = isActive;
      } else {
        Serial.print(F(" FAIL"));
      }
    }
  } else {
    stableCount = 0;
    lastStableStatus = isActive;
    Serial.print(F(" (reset)"));
  }

  Serial.println();
}
