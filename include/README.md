# Aquarium Controller — OTA Stub

## Files in this drop

- `main.cpp` — replaces your existing `src/main.cpp`. Pin reassignments + OTA hooks, no UI/logic changes.
- `wifi_ota.h` / `wifi_ota.cpp` — new module. Drop in `src/` alongside `main.cpp`.
- `config.h.example` — template for credentials. **Copy to `config.h` and fill in.** Goes in `src/`.
- `platformio.ini` — replaces existing. Adds OTA environment and lib deps.
- `.gitignore` — ensures `config.h` never gets committed.

## First flash (over USB)

1. Copy `config.h.example` → `src/config.h` and fill in your WiFi SSID/password.
2. Plug in USB-C.
3. In VS Code / PlatformIO, make sure environment is `serial` (it's the default).
4. Hit **Upload**. Open the serial monitor at 115200.
5. You should see:
   ```
   === Aquarium Controller ===
   [WiFi] Connecting to "your-ssid"...
   [WiFi] Connected. IP: 192.168.x.x  RSSI: -xx dBm
   [OTA] Ready. Hostname: aquarium.local  IP: 192.168.x.x
   ```

## Subsequent flashes (over WiFi)

Option A — switch the default environment:
- Edit `platformio.ini`, change `default_envs = serial` to `default_envs = ota`.
- Hit **Upload** normally. It'll push the binary over WiFi to `aquarium.local`.

Option B — keep serial as default, use CLI for OTA:
```
pio run -e ota -t upload
```

If mDNS (`.local`) is flaky on your network, edit `platformio.ini` and
hardcode `upload_port = <the IP printed on serial>` in the `[env:ota]` section.

## Pin assignment summary

| Function | Pin | Notes |
|---|---|---|
| TFT_CS / TFT_DC | 15 / 33 | FeatherWing defaults, unchanged |
| TEMP (OneWire) | 14 | unchanged |
| LIGHTS_RELAY | **13** | was 12 |
| HEATER_RELAY | **12** | was 27 (strap pin — code forces LOW at boot) |
| FILTER_RELAY | **27** | was 33 (which collided with TFT_DC) |
| LIGHTS_SENSE | **34 (A2)** | was 26 — moved to ADC1 so WiFi doesn't break it |
| HEATER_SENSE | **39 (A3)** | was 25 — moved to ADC1 |
| FILTER_SENSE | **36 (A4)** | was 34 — renumbered for order consistency |

## Smoke test checklist after first flash

- [ ] Serial prints boot banner and WiFi connection info
- [ ] Display comes up, thermostat screen renders
- [ ] Touch still responsive (temp +/- and LIGHTS button)
- [ ] **Heater relay**: force setpoint above current temp, confirm click. This is the relay whose pin changed from 27 → 12.
- [ ] Lights relay toggles from the LIGHTS screen (onboard red LED should mirror it)
- [ ] Filter relay: we haven't exercised this in code yet, but it's wired to 27 now and initialized LOW
- [ ] `ping aquarium.local` works from your laptop
- [ ] Try a trivial OTA upload (e.g., change a Serial.println text) to confirm the wireless pipeline works

## What's next

With OTA in place, the remaining phases from your list can all ship wirelessly:
1. UI simplification (single status screen)
2. Display sleep / touch-to-wake
3. Touch light override (5 min timer)
4. Current sensor bring-up
5. Web interface (ElegantOTA will slot in here too for browser-based uploads as a backup)
