# Big NTP Clock

A large 7-segment LED clock built on a Seeed Studio XIAO ESP32-C3, driven by NTP for accurate timekeeping. Features a web configuration interface, Home Assistant integration via MQTT discovery, and a hot tub temperature display.

## Hardware

| Component | Detail |
|---|---|
| Microcontroller | Seeed Studio XIAO ESP32-C3 |
| Clock digit strip | 252 LEDs, NEO_GRB, connected to D3 (GPIO5) |
| Downlight strip | 14 LEDs, NEO_GRB, connected to D2 (GPIO4) |
| Light sensor | LDR on A0 (GPIO2 / ADC1, 12-bit) |
| PSU | Meanwell 60W 5V |

### Segment Layout

Each digit consists of 63 pixels across 7 segments (9 pixels per segment):

```
 ─────      segments 9-17   (top)
│     │     segments 18-26  (left-top)    segments 0-8   (right-top)
 ─────      segments 27-35  (middle)
│     │     segments 45-53  (left-bottom) segments 36-44 (right-bottom)
 ─────      segments 54-62  (bottom)
```

Digit bases (pixel offset): `{189, 126, 63, 0}` — right to left in memory, left to right visually.

## Features

- **NTP time sync** — syncs to `0.uk.pool.ntp.org`, updates every minute
- **Auto BST/GMT** — automatically switches on the last Sunday of March/October
- **Manual timezone override** — force GMT or BST via web or MQTT
- **Auto brightness** — LDR with 8-sample rolling average scales display brightness
- **Persistent config** — colours and timezone settings survive reboots (stored in flash via Preferences)
- **OTA updates** — firmware updates over WiFi via PlatformIO or ArduinoOTA
- **Web interface** — live colour control at `http://bigclock.local`
- **Secret effects page** — rainbow animation and colour presets at `http://bigclock.local/fx` (double-tap the clock display)
- **MQTT discovery** — auto-creates entities in Home Assistant
- **Hot tub temperature** — displays temperature for 5 seconds every 10 seconds (received via MQTT)

## Web Interface

### Main page (`/`)
- Live colour pickers for hour digits, minute digits, and downlights (updates on drag, no save button)
- Auto BST/GMT toggle with manual override
- BST changeover dates for the current year

### Effects page (`/fx`) — double-tap the clock display to access
- Colour pickers for hour and minute digits
- Randomise button — picks vivid random colours
- Rainbow wave toggle
- 8 colour presets: Fire, Ice, Neon, Sunset, Ocean, Mono, Candy, Midnight

## MQTT / Home Assistant Integration

The clock uses MQTT discovery to automatically create the following entities in Home Assistant under **Settings → Devices → Big Clock**:

| Entity | Type |
|---|---|
| Hour Colour | Light (RGB) |
| Minute Colour | Light (RGB) |
| Downlight Colour | Light (RGB) |
| Auto BST | Switch |
| Timezone | Select (GMT / BST) |
| Rainbow | Switch |
| Randomise | Button |
| Preset: Fire/Ice/Neon/Sunset/Ocean/Mono/Candy/Midnight | Button × 8 |a

### Hot Tub Temperature

The clock subscribes to `bigclock/hottub`. Any MQTT message to that topic containing a numeric value (e.g. `22.25`) will be stored and displayed as an integer (floored, not rounded).

To publish from Home Assistant, create an automation:

```yaml
alias: Publish HotTub Temp to BigClock
triggers:
  - trigger: state
    entity_id: sensor.your_temperature_sensor
actions:
  - action: mqtt.publish
    data:
      evaluate_payload: true
      topic: bigclock/hottub
      payload: "{{ states('sensor.your_temperature_sensor') }}"
```

### MQTT Topics

| Topic | Direction | Description |
|---|---|---|
| `bigclock/availability` | publish | `online` / `offline` |
| `bigclock/state` | publish | JSON state (colours, BST, rainbow) |
| `bigclock/cmd` | subscribe | JSON commands (presets, rainbow, random, BST) |
| `bigclock/cmd/hc` | subscribe | Hour colour (HA light format) |
| `bigclock/cmd/mc` | subscribe | Minute colour (HA light format) |
| `bigclock/cmd/dc` | subscribe | Downlight colour (HA light format) |
| `bigclock/hottub` | subscribe | Temperature value (numeric string) |

## Setup

### Prerequisites

- [VS Code](https://code.visualstudio.com) with the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode)
- Mosquitto MQTT broker (running as a Home Assistant add-on or standalone)

### Configuration

Edit `src/main.cpp` and update the following constants near the top of the file:

```cpp
const char* WIFI_SSID  = "your_wifi_ssid";
const char* WIFI_PASS  = "your_wifi_password";
const char* MQTT_HOST  = "homeassistant.local";
const char* MQTT_USER  = "your_mqtt_username";
const char* MQTT_PASS  = "your_mqtt_password";
```

### First Flash (USB)

1. Open the project folder in VS Code
2. PlatformIO will automatically install all libraries on first build
3. In `platformio.ini`, comment out the OTA upload lines and uncomment the USB lines:
   ```ini
   ; upload_protocol = espota
   ; upload_port     = 192.168.0.122
   ; upload_flags    = --auth=

   upload_protocol = esptool
   upload_port     = /dev/cu.usbmodem101
   upload_speed    = 921600
   ```
4. Connect the XIAO via USB-C, click the Upload button (→) in PlatformIO

### Subsequent Updates (OTA)

Switch `platformio.ini` back to OTA mode, update `upload_port` to match the clock's current IP, and click Upload. Find the IP with:

```bash
ping bigclock.local
```

### Serial Monitor

```bash
pio device monitor --port /dev/cu.usbmodem101
```

## Project Structure

```
BigClock/
├── platformio.ini    ← project config, libraries, upload settings
├── src/
│   └── main.cpp      ← full firmware
├── include/          ← headers (unused)
├── .gitignore
└── README.md
```

## Known Limitations

- WiFi credentials and MQTT credentials are stored in plaintext in `src/main.cpp` — do not commit to a public repository without removing them first
- Rainbow animation visual behaviour (sweeping colour wave) is implemented but the exact column mapping may need tuning for your specific digit layout
- OTA upload via PlatformIO takes ~75 seconds due to the ArduinoOTA protocol speed
