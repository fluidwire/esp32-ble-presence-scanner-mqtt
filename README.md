# esp32-ble-presence-scanner-mqtt — report nearby BLE device presence from an ESP32 over MQTT

IoT example by [Fluidwire](https://fluidwire.com/).

Sample sketch for an ESP32 dev board that scans for Bluetooth Low Energy devices every 30 seconds, matches what it hears against a **whitelist of MAC addresses** you provide (phones, fitness bands, BLE tags, other ESP32 beacons), and publishes a `home` / `away` state for each one over MQTT. No extra hardware — the ESP32's own radio does the scanning while WiFi stays connected.

It is a cheap way to prototype room-occupancy sensing, a lab or classroom attendance board, or an "is anyone home?" input for a Home Assistant automation — the kind of thing that turns up in a lot of thesis projects.

## What it does

On boot it starts the NimBLE stack in scan-only mode, connects to WiFi, then connects to the MQTT broker and publishes every tracked device as `away`. Every 30 seconds it runs a 5-second active scan, compares each advertiser's address against the whitelist and records the RSSI of any match. A device becomes `home` the first scan it is heard; it only becomes `away` after being **missed 3 scans in a row**, so a single dropped packet does not make the state flap. Per-device states are published **retained**, and a JSON summary of every scan goes to `<topic>/scan`.

## Required hardware

- 1x ESP32 dev board (classic ESP32 — the sketch needs the BLE radio, which the ESP32-S2 lacks)
- At least one BLE device to track (a phone, a BLE tag, or another ESP32 running [esp32-ble-sensor-beacon](https://github.com/fluidwire/esp32-ble-sensor-beacon))
- An MQTT broker (local Mosquitto, or a public test broker like `broker.hivemq.com`)
- A 2.4 GHz WiFi network

## Required libraries

Install via Library Manager:

- **NimBLE-Arduino** by h2zero (v2.x — a lightweight BLE stack that leaves room for the WiFi stack)
- **PubSubClient** by Nick O'Leary

## Setup

1. Copy `config.h.example` → `config.h`. Fill in your WiFi + MQTT broker details.
2. Find the BLE MAC address of each device you want to track with a scanner app such as **nRF Connect** or **LightBlue**, and add one `{ "mac", "name" }` pair per device to `TRACKED_DEVICES` in `config.h`. The `name` becomes part of the MQTT topic, so keep it lower-case with no spaces.
3. Flash. Open the Serial Monitor at **115200 baud**.

No wiring is needed.

## Expected serial output

```
[ble] tracking 2 device(s)
[wifi] connecting to my-network
....
[wifi] IP: 192.168.1.42
[mqtt] connecting... ok
[presence] ton-phone -> away
[presence] lab-tag -> away
[ble] scanning... 14 advertisers
[ble]   match ton-phone rssi=-67
[presence] ton-phone -> home
[mqtt] {"seen":1,"devices":[{"name":"ton-phone","home":true,"rssi":-67},{"name":"lab-tag","home":false,"rssi":0}],"uptime_s":12}
```

Subscribe from your machine to watch the data arrive:

```
mosquitto_sub -h broker.hivemq.com -t "fluidwire/demo/presence/#" -v
```

## Topics

| Topic | Payload | Retained |
|-------|---------|----------|
| `fluidwire/demo/presence/<name>` | `home` or `away` | yes |
| `fluidwire/demo/presence/scan` | `{"seen":1,"devices":[...],"uptime_s":12}` | no |

## Troubleshooting

- **My phone is never detected** — modern iPhones and many Android phones **randomise their BLE address** and only advertise while a BLE app is open. For reliable tracking use a BLE tag, a fitness band, or an ESP32 beacon with a fixed address; or pair your phone's fixed address via a companion app. The whitelist approach works best with devices you control.
- **`0 advertisers` every scan** — the board has no BLE radio (ESP32-S2) or BLE is disabled in the board's menuconfig. Use a classic ESP32 / ESP32-C3 / ESP32-S3.
- **Sketch too big / won't fit** — pick the `Huge APP (3MB No OTA)` or `Minimal SPIFFS` partition scheme in Tools; WiFi + BLE together need the space. NimBLE is already much smaller than the Bluedroid stack.
- **State flaps between home and away** — the device is at the edge of range. Move the ESP32, lower `SCAN_INTERVAL_MS`, or raise `AWAY_AFTER_MISSES` in the sketch.
- **`failed state=-2`** on MQTT — the broker host/port is wrong or unreachable.
- **Summary never arrives but states do** — the JSON summary is larger than PubSubClient's default buffer. The sketch already raises it to 512 bytes; if you track many devices, raise `setBufferSize` and the `buf` size in `publishSummary()`.

## Security note

This sketch keeps your WiFi and MQTT credentials **and the tracked MAC addresses** in `config.h`, which is `.gitignore`'d — it never gets committed. Only `config.h.example` ships in the repo. Never hardcode real SSID/password into the `.ino`. Presence data is personal: on a public broker anyone subscribed to your topic can see who is home, so use a private broker with `MQTT_USER`/`MQTT_PASSWORD` (and TLS — see [esp32-mqtt-tls-secure](https://github.com/fluidwire/esp32-mqtt-tls-secure)) for anything beyond a demo.

## License

MIT — see [LICENSE](LICENSE).

## Got stuck?

Drop by [fluidwire.com](https://fluidwire.com/) — happy to help.
