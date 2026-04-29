# BirksBeamer Aquarium Controller

A full-featured aquarium automation controller built on the Adafruit ESP32 Feather V2 platform. Controls and monitors temperature, lighting schedule, and filtration — with a 3.5" color touchscreen, live web dashboard, over-the-air firmware updates, and SD card logging.

---

<!-- TODO: add a photo of the completed display enclosure here -->
<!-- ![Controller mounted above the tank](docs/images/controller-mounted.jpg) -->

---

## Overview

This started as a simple relay timer and grew into a complete monitoring and control system. The goals were:

- **Reliable automation** — lights, heater, and filter should just work on a schedule without babysitting
- **Visibility** — a glanceable display showing temperature, relay states, current draw, and schedule at all times
- **Remote access** — a clean web UI for configuration and log tailing from anywhere on the LAN
- **Fault detection** — current sensing on all three loads catches wiring failures, relay failures, and load surprises
- **Maintainability** — OTA updates, SD-logged history, and a design simple enough to reason about

Development went through several phases: initial proof of concept with basic relay control, hardware validation with touch UI and current sensing, a full UI redesign with web configuration and current averaging, and ongoing refinements to scheduling reliability and touch sensitivity.

---

## Features

- **Temperature control** — DS18B20 waterproof probe, configurable set point and hysteresis, sparkline history
- **Light scheduling** — NTP-synced on/off times (default 9 am – 3 pm Pacific); manual 5-minute override via touchscreen
- **Filter control** — normally-closed relay keeps filter running through power glitches
- **Current sensing** — non-invasive AC current sensors on all three loads; averaged fault detection flags mismatches between relay state and actual current
- **3.5" color touchscreen** — dark-themed status display; tap to override lights, initiate water changes, or wake from sleep
- **Web dashboard** — live status, temperature chart, log stream, and all configurable parameters
- **Water change tracking** — log changes from the touchscreen or web UI; on-screen reminder as the due date approaches
- **OTA firmware updates** — flash new firmware over WiFi without physical access
- **SD card logging** — timestamped CSV of temperature, relay states, and current readings

---

## Hardware

### Bill of Materials

| Component | Source | Est. Cost |
|---|---|---|
| [Adafruit Feather ESP32 V2](https://www.adafruit.com/product/5400) | Adafruit | $22.95 |
| [Adafruit 3.5" TFT FeatherWing](https://www.adafruit.com/product/3651) | Adafruit | $44.95 |
| DS18B20 waterproof temp sensor + cable | Amazon / Adafruit | $5.00 |
| 3-channel 5V relay module (active HIGH) | Amazon | $8.00 |
| 3× YHDC SCT-013-030 non-invasive current sensor | Amazon | $18.00 |
| 3× 3.5 mm stereo panel-mount jacks (for current sensors) | Amazon | $3.00 |
| 10 kΩ resistor (DS18B20 pull-up) | — | $0.10 |
| Wiring, JST connectors, pin headers, misc | — | ~$8.00 |
| **Total (with display)** | | **~$110** |

The display accounts for nearly half the cost. A **web-only variant** — same ESP32 and sensors, no screen — can be built for roughly **$60–65**, with all monitoring and control accessible through the web dashboard.

### Pin Assignments

| Signal | GPIO |
|---|---|
| TFT chip select | 15 |
| TFT data/command | 33 |
| TFT backlight | 25 |
| Touch controller | I2C (TSC2007, addr 0x48) |
| SD card CS | 14 |
| Temperature sensor (OneWire) | 26 |
| Lights relay (active HIGH) | 13 |
| Heater relay (active HIGH) | 12 |
| Filter relay (normally-closed) | 27 |
| Lights current sense (ADC1) | 34 |
| Heater current sense (ADC1) | 39 |
| Filter current sense (ADC1) | 36 |

> **Note:** GPIO 12 is a strap pin on the ESP32 and must read LOW at boot. The heater relay drive line is held LOW before `pinMode()` is set to prevent glitches.

### Wiring Diagram

> 📐 *Wiring diagram coming soon — to be added as a KiCad schematic or Fritzing export in `docs/wiring/`.*

---

## Enclosures

The build uses two 3D-printed enclosures designed in Autodesk Fusion 360:

1. **Display enclosure** — houses the Feather + TFT FeatherWing stack; designed for wall or shelf mounting above the tank
2. **Relay enclosure** — houses the relay module and current sensor jacks; mounted near the power strip

> 📦 *Fusion 360 source files and STL exports coming soon — to be published in `docs/enclosures/`.*

---

## Photos

> 📷 *Photos coming soon.*

<!-- Suggested shots to add:
  - docs/images/display-enclosure.jpg  — assembled display unit
  - docs/images/relay-enclosure.jpg    — relay and current sensor wiring
  - docs/images/web-dashboard.jpg      — web UI screenshot
  - docs/images/tank-overview.jpg      — full installation on the tank
-->

---

## Software Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- WiFi credentials in `src/wifi_ota.cpp` (see that file for the `WIFI_SSID` / `WIFI_PASS` defines)

### First Flash (USB)

```bash
pio run -e serial -t upload
```

### Subsequent Updates (OTA)

Once the device has connected to WiFi and obtained an IP, all future flashes can go over the air:

```bash
pio run -e ota -t upload
```

The device advertises itself as `aquarium.local` via mDNS. If mDNS is unreliable on your network, set `upload_port` to the device's IP address in `platformio.ini`.

### Web Interface

Browse to `http://aquarium.local` after the device boots. From there you can:

- Adjust set point, hysteresis, and schedule hours
- Set current-sensor fault thresholds and averaging window
- Configure water change interval and log a change
- View the live temperature chart and log stream

All settings persist across reboots in ESP32 NVS flash.

---

## Configuration Defaults

| Parameter | Default |
|---|---|
| Temperature set point | 78.0 °F |
| Heater hysteresis | ± 0.3 °F |
| Lights on | 9:00 am |
| Lights off | 3:00 pm |
| Timezone | US Pacific (PST8PDT) |
| Display sleep timeout | 5 minutes |
| Manual light override | 5 minutes |
| Water change interval | 14 days |
| Current averaging window | 10 samples (20 s) |

---

## Project Structure

```
src/
  main.cpp        — display, control loop, touch handling
  web_ui.cpp      — async HTTP server, settings, SSE log stream
  wifi_ota.cpp    — WiFi connection, OTA, logging
  sd_logger.cpp   — SD card CSV logging
include/
  web_ui.h
  wifi_ota.h
  sd_logger.h
```

---

## Planned Additions

- [ ] Fusion 360 source files and STL exports for both enclosures
- [ ] Wiring diagram / schematic
- [ ] Build photos
- [ ] Web-only (no-display) variant guide with cost breakdown
- [ ] pH probe integration (Atlas Scientific EZO)
