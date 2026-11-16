/*
 * ESP32 BLE Presence Scanner -> MQTT
 * ----------------------------------
 * Periodically scans for nearby Bluetooth Low Energy devices, matches the
 * results against a whitelist of known MAC addresses (phones, fitness bands,
 * BLE tags, other ESP32 beacons) and publishes a "home" / "away" presence
 * state per device over MQTT. Useful for room-occupancy demos, lab or
 * classroom attendance prototypes and "is anyone home?" automations.
 *
 * The sketch shows:
 *   - running a periodic, blocking BLE scan with NimBLE while WiFi + MQTT
 *     stay connected
 *   - matching advertisers against a whitelist of MAC addresses
 *   - debounced presence (a device must be missed several scans in a row
 *     before it is declared away) so one dropped packet does not flap
 *   - retained per-device state topics plus a JSON summary of every scan
 *
 * Required libraries (install via Library Manager):
 *   - "NimBLE-Arduino" by h2zero  (v2.x; lightweight BLE stack that leaves
 *     room for the WiFi stack)
 *   - "PubSubClient" by Nick O'Leary
 *
 * Hardware:
 *   - ESP32 dev board only (the built-in radio does the scanning)
 *
 * Setup:
 *   1. Copy config.h.example to config.h
 *   2. Fill in WiFi + MQTT broker details and the MAC addresses to track.
 *   3. Flash. Open Serial Monitor at 115200.
 *
 * MQTT topics (MQTT_TOPIC = "fluidwire/demo/presence"):
 *   fluidwire/demo/presence/<name>   "home" | "away"   (retained)
 *   fluidwire/demo/presence/scan     {"seen":2,"devices":[...],"uptime_s":123}
 *
 * (c) 2026 Fluidwire. Released under the MIT License.
 * https://github.com/fluidwire
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>
#include "config.h"

const uint32_t SCAN_INTERVAL_MS  = 30UL * 1000UL; // how often we scan
const uint32_t SCAN_DURATION_MS  = 5UL * 1000UL;  // how long each scan listens
const uint8_t  AWAY_AFTER_MISSES = 3;             // scans in a row missed before "away"

struct Tracked {
  const char* mac;   // lower-case "aa:bb:cc:dd:ee:ff"
  const char* name;  // topic-safe label, e.g. "ton-phone"
};

// Whitelist comes from config.h so MAC addresses never end up in the repo.
const Tracked TRACKED[] = { TRACKED_DEVICES };
const size_t  TRACKED_COUNT = sizeof(TRACKED) / sizeof(TRACKED[0]);

struct Presence {
  bool    home;
  uint8_t misses;
  int8_t  rssi;
  bool    seenThisScan;
};
Presence presence[sizeof(TRACKED) / sizeof(TRACKED[0])];

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
NimBLEScan*  pScan = nullptr;

void connectWiFi() {
  Serial.print(F("[wifi] connecting to "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  Serial.print(F("[wifi] IP: "));
  Serial.println(WiFi.localIP());
}

void connectMQTT() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512); // the scan summary can exceed the 256-byte default
  while (!mqtt.connected()) {
    Serial.print(F("[mqtt] connecting... "));
    bool ok = strlen(MQTT_USER) > 0
      ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)
      : mqtt.connect(MQTT_CLIENT_ID);
    if (ok) {
      Serial.println(F("ok"));
    } else {
      Serial.print(F("failed state="));
      Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

// Case-insensitive compare so "AA:BB:..." in config.h still matches.
bool macEquals(const char* a, const char* b) {
  return strcasecmp(a, b) == 0;
}

int findTracked(const char* mac) {
  for (size_t i = 0; i < TRACKED_COUNT; i++) {
    if (macEquals(TRACKED[i].mac, mac)) return (int)i;
  }
  return -1;
}

void publishState(size_t i) {
  char topic[128];
  snprintf(topic, sizeof(topic), "%s/%s", MQTT_TOPIC, TRACKED[i].name);
  const char* state = presence[i].home ? "home" : "away";
  Serial.print(F("[presence] "));
  Serial.print(TRACKED[i].name);
  Serial.print(F(" -> "));
  Serial.println(state);
  mqtt.publish(topic, state, true); // retained: dashboards see the last state at once
}

void publishSummary(uint32_t now) {
  // {"seen":2,"devices":[{"name":"ton-phone","home":true,"rssi":-67},...],"uptime_s":123}
  char buf[480];
  size_t len = 0;
  uint8_t seen = 0;
  for (size_t i = 0; i < TRACKED_COUNT; i++) if (presence[i].seenThisScan) seen++;

  len += snprintf(buf + len, sizeof(buf) - len, "{\"seen\":%u,\"devices\":[", seen);
  for (size_t i = 0; i < TRACKED_COUNT && len < sizeof(buf) - 64; i++) {
    len += snprintf(buf + len, sizeof(buf) - len,
                    "%s{\"name\":\"%s\",\"home\":%s,\"rssi\":%d}",
                    i ? "," : "", TRACKED[i].name,
                    presence[i].home ? "true" : "false",
                    presence[i].seenThisScan ? presence[i].rssi : 0);
  }
  snprintf(buf + len, sizeof(buf) - len, "],\"uptime_s\":%lu}",
           (unsigned long)(now / 1000UL));

  char topic[128];
  snprintf(topic, sizeof(topic), "%s/scan", MQTT_TOPIC);
  Serial.print(F("[mqtt] "));
  Serial.println(buf);
  mqtt.publish(topic, buf);
}

void runScan(uint32_t now) {
  for (size_t i = 0; i < TRACKED_COUNT; i++) presence[i].seenThisScan = false;

  Serial.print(F("[ble] scanning... "));
  // Blocking scan: returns after SCAN_DURATION_MS with everything heard.
  NimBLEScanResults results = pScan->getResults(SCAN_DURATION_MS, false);
  Serial.print(results.getCount());
  Serial.println(F(" advertisers"));

  for (int n = 0; n < results.getCount(); n++) {
    const NimBLEAdvertisedDevice* d = results.getDevice(n);
    std::string mac = d->getAddress().toString();
    int idx = findTracked(mac.c_str());
    if (idx < 0) continue;
    presence[idx].seenThisScan = true;
    presence[idx].rssi = d->getRSSI();
    Serial.print(F("[ble]   match "));
    Serial.print(TRACKED[idx].name);
    Serial.print(F(" rssi="));
    Serial.println(d->getRSSI());
  }
  pScan->clearResults(); // free the memory held by the result list

  // Debounce: arrive immediately, leave only after N consecutive misses.
  for (size_t i = 0; i < TRACKED_COUNT; i++) {
    Presence& p = presence[i];
    if (p.seenThisScan) {
      p.misses = 0;
      if (!p.home) { p.home = true; publishState(i); }
    } else {
      if (p.misses < 255) p.misses++;
      if (p.home && p.misses >= AWAY_AFTER_MISSES) { p.home = false; publishState(i); }
    }
  }

  publishSummary(now);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  for (size_t i = 0; i < TRACKED_COUNT; i++) {
    presence[i] = { false, AWAY_AFTER_MISSES, 0, false };
  }

  // Empty device name: we only scan, we never advertise.
  NimBLEDevice::init("");
  pScan = NimBLEDevice::getScan();
  pScan->setActiveScan(true);     // request scan responses (more names, slightly more power)
  pScan->setInterval(100);        // ms between scan windows
  pScan->setWindow(99);           // ms listening per interval (~continuous)
  pScan->setDuplicateFilter(true);
  Serial.print(F("[ble] tracking "));
  Serial.print(TRACKED_COUNT);
  Serial.println(F(" device(s)"));

  connectWiFi();
}

void loop() {
  static uint32_t lastScan = 0;
  static bool     firstScanDone = false;

  if (!mqtt.connected()) {
    connectMQTT();
    // (Re)publish every state on connect so retained topics are correct.
    for (size_t i = 0; i < TRACKED_COUNT; i++) publishState(i);
  }
  mqtt.loop();

  uint32_t now = millis();
  if (firstScanDone && now - lastScan < SCAN_INTERVAL_MS) {
    delay(20);
    return;
  }
  lastScan = now;
  firstScanDone = true;

  runScan(now);
}
