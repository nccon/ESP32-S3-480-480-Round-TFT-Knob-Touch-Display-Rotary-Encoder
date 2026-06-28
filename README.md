# VIEWE Knob Display — Home Assistant Dashboard

A multi-page Home Assistant dashboard for the **VIEWE UEDX48480021-MD80ET** — a
2.1" 480×480 round touch display with a rotary encoder, built on the ESP32-S3.
Rotate the knob to flip pages, tap to interact. Integrates with Home Assistant
over MQTT with auto-discovery.

![display](docs/display.jpg) <!-- add your own photo -->

---

## Features

- **Four pages**, navigated by rotating the knob (dots show your position):
  - **Dashboard** — temperature, weather, alarm status
  - **Alarm** — Arm Away / Arm Home / Disarm (tap by touch)
  - **Doorbell** — refreshing JPEG snapshot from a HA camera (knob press = refresh)
  - **Info** — IP address, WiFi signal, MQTT status
- **MQTT auto-discovery** — the device appears in Home Assistant on its own as
  "Knob Display" with diagnostic entities; no manual configuration.
- **Two-way** — Home Assistant pushes live values to the screen; the screen
  sends alarm commands back.
- **Responsive UI** — all networking runs on a dedicated core, so page changes
  are instant regardless of MQTT/network state.
- Capacitive touch and rotary encoder both work.

---

## ⚠️ Important: why this is an Arduino project, not ESPHome

This panel uses a GC9503/ST7701S RGB controller whose SPI initialization lines
(**GPIO12/13**) are *also* two of the RGB data lines. The display driver must
delete the SPI bus after sending init commands so those pins can carry RGB data.

**ESPHome's driver does not do this**, so on ESPHome the panel initializes, the
backlight comes on, and LVGL runs — but **no pixels ever reach the screen**
(black display). See [esphome/esphome#13140](https://github.com/esphome/esphome/issues/13140).
This is not fixable from YAML and affects all current ESPHome versions.

The VIEWE **ESP32_Display_Panel** library handles the SPI→RGB hand-off correctly
(you'll see `Delete but keep CS line inactive` in the boot log), which is why
this project uses the Arduino/ESP-IDF path instead.

---

## Hardware

| | |
|---|---|
| Board | VIEWE UEDX48480021-MD80ET |
| MCU | ESP32-S3, 240 MHz, dual core |
| RAM | 8 MB PSRAM (octal) |
| Flash | 16 MB |
| Display | 2.1" 480×480 round IPS, GC9503/ST7701S, 16-bit RGB |
| Touch | CST826 (I²C) |
| Input | Rotary encoder + push button |

Official board repo: <https://github.com/VIEWESMART/UEDX48480021-MD80ESP32-2.1inch-Touch-Knob-Display>

### Pin reference

| Function | GPIO | Function | GPIO |
|---|---|---|---|
| Backlight | 7 | Touch SDA | 16 |
| LCD reset | 8 | Touch SCL | 15 |
| SPI CS | 18 | Encoder A | 6 |
| SPI SCK | 13 (shared w/ RGB DATA3) | Encoder B | 5 |
| SPI SDA | 12 (shared w/ RGB DATA2) | Encoder button | 0 |

---

## Prerequisites

### Arduino IDE setup
- **ESP32 board package** ≥ 3.1.0
- Libraries (Library Manager):
  - `ESP32_Display_Panel` (≥ 1.0.3 — click *Install All* for dependencies)
  - `lvgl` (8.4.0)
  - `ESP32Encoder` (by madhephaestus)
  - `PubSubClient` (by Nick O'Leary)
  - `TJpg_Decoder` (by Bodmer)

### Board configuration

In the `ESP32_Display_Panel` library, edit `esp_panel_board_supported_conf.h`:
```c
#define ESP_PANEL_BOARD_DEFAULT_USE_SUPPORTED   (1)
#define BOARD_VIEWE_UEDX48480021_MD80ET            // uncomment this line
```

Create an `lv_conf.h` (copy `lv_conf_template.h` to the `libraries/` folder, one
level above `lvgl/`, and rename). Enable it and set:
```c
#if 1                              // enable the file
#define LV_COLOR_DEPTH    16
#define LV_COLOR_16_SWAP  0        // RGB panel — required
#define LV_FONT_MONTSERRAT_28  1   // used by the UI
```

### Arduino IDE → Tools
| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| PSRAM | OPI PSRAM |
| USB CDC On Boot | Enabled *(for serial debug output)* |

---

## Build & flash

1. Open `File → Examples → ESP32_Display_Panel → Arduino → gui → lvgl_v8 →
   simple_port`, then **File → Save As** to a new folder. This brings the
   board-specific `lvgl_v8_port.h` and `lvgl_v8_port.cpp` along.
2. Replace the example's `.ino` with `knob_dashboard_mqtt.ino` from this repo.
3. Confirm the sketch folder contains: `knob_dashboard_mqtt.ino`,
   `lvgl_v8_port.h`, `lvgl_v8_port.cpp`.
4. Fill in the **USER CONFIG** block (see below).
5. Compile and upload over USB (hold **BOOT** if the upload won't start).

---

## Configuration

Edit the `USER CONFIG` block at the top of `knob_dashboard_mqtt.ino`:

```cpp
WIFI_SSID / WIFI_PASS     // your WiFi
MQTT_HOST                 // your HA / Mosquitto IP (e.g. 192.168.10.159)
MQTT_USER / MQTT_PASS     // a Home Assistant user created for the device
HA_HOST                   // http://<HA-ip>:8123  (local, NOT the https hostname)
HA_TOKEN                  // HA long-lived access token (for the camera)
CAM_ENTITY                // your doorbell camera entity, e.g. camera.front_door
```

> Use HA's **local** `http://ip:8123` for `HA_HOST`, not a Cloudflare/HTTPS
> hostname — plain `HTTPClient` doesn't do TLS.

---

## Home Assistant setup

1. Install the **Mosquitto broker** add-on and the **MQTT** integration.
2. Create a Home Assistant **user** for the device (Settings → People → Users)
   and use those credentials as `MQTT_USER` / `MQTT_PASS`.
3. Add the automations in `knob_ha_automations.yaml`. They:
   - publish your temperature / weather / alarm state to the topics the screen
     subscribes to (`knob/set/*`), and
   - translate the screen's alarm commands (`knob/cmd/alarm`) into service calls.

   Paste the file into `automations.yaml` (File Editor or Studio Code Server
   add-on), edit the entity IDs marked `<<<`, then **Developer Tools → YAML →
   Reload Automations**.

Without these automations the dashboard shows `--`, because the device only
listens — Home Assistant does not publish entity states to MQTT by default.

### MQTT topics

| Topic | Direction | Purpose |
|---|---|---|
| `knob/status` | device → | availability (LWT) |
| `knob/set/temperature` | → device | temperature value |
| `knob/set/weather` | → device | weather condition |
| `knob/set/alarm` | → device | alarm state |
| `knob/cmd/alarm` | device → | `ARM_AWAY` / `ARM_HOME` / `DISARM` |
| `knob/ip`, `knob/rssi`, `knob/page`, `knob/action` | device → | diagnostics |

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| **Black screen, backlight only** (ESPHome) | Known driver bug #13140 — use this Arduino firmware instead. |
| `lv_conf.h: No such file` | Create `lv_conf.h` in `libraries/` and enable it (`#if 1`). |
| `lvgl_v8_port.h: No such file` | Copy it + the `.cpp` from the `simple_port` example into the sketch folder. |
| `lv_font_montserrat_28 not declared` | Set `LV_FONT_MONTSERRAT_28 1` in `lv_conf.h`. |
| `No default board configuration detected` → panic | Enable the board in `esp_panel_board_supported_conf.h` (both lines). |
| `text section exceeds available space` | Use the *16M Flash (3MB APP/...)* partition scheme. |
| Serial shows only board logs, none of your prints | Set **USB CDC On Boot: Enabled**. |
| Page changes laggy | All networking is on core 0 in this firmware; make sure `MQTT_PASS` is correct so it isn't retrying. |
| Camera: colors wrong | Toggle `TJpgDec.setSwapBytes(true/false)`. |
| Camera: `Bad size` | Snapshot exceeds the 250 KB cap — point `CAM_ENTITY` at a lower-res substream. |
| Alarm won't arm/disarm | Most panels need a code — add `data: { code: "1234" }` to the automation service calls. |

---

## Limitations

- The doorbell page shows **periodic still snapshots**, not live video — this
  MCU can't decode an RTSP/H.264 stream in real time.
- Large camera resolutions decode slowly; use a low-res snapshot or substream.

---

## Credits

- [VIEWE / VIEWESMART](https://github.com/VIEWESMART) — board and driver libraries
- [LVGL](https://lvgl.io) — UI framework
- [esp-arduino-libs/ESP32_Display_Panel](https://github.com/esp-arduino-libs/ESP32_Display_Panel)
- TJpg_Decoder (Bodmer), PubSubClient (Nick O'Leary), ESP32Encoder (madhephaestus)

## License

MIT — see [LICENSE](LICENSE).
