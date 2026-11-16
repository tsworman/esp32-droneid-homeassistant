# ESP32-S3-ETH Remote ID Drone Scanner & MQTT Relay

A dual-radio (Wi-Fi + Bluetooth Low Energy) ASTM F3411 / OpenDroneID receiver running on a **Waveshare ESP32-S3-ETH** with a PoE shield. It captures drone Remote ID broadcasts over the air and relays parsed JSON telemetry (drone ID, GPS coordinates, altitude, speed, heading, and pilot location) over wired Ethernet to a Home Assistant MQTT broker.

Assisted by Gemini

---

## Hardware Requirements & Modifications

* **Microcontroller:** Waveshare ESP32-S3-ETH (ESP32-S3R8 + W5500 SPI Ethernet controller)
* **Power (Optional):** Waveshare PoE Module/Shield (IEEE 802.3af/at compliant)
* **Antenna:** 2.4 GHz External IPEX/U.FL Antenna

### Hardware Antenna Modification (Required for External Antenna)
By default, the Waveshare ESP32-S3-ETH routes the 2.4 GHz RF path to the onboard ceramic chip antenna. To enable the external U.FL/IPEX connector for improved Wi-Fi and BLE range:
1. Locate the `0Ω` SMD resistor near the antenna feed line / U.FL connector on the PCB.
2. **Desolder/remove** the `0Ω` resistor from the onboard ceramic antenna pads.
3. **Bridge/solder** the `0Ω` resistor (or a solder bridge) across the pads leading to the external IPEX/U.FL connector.
4. Connect a 2.4 GHz antenna before powering on the board.

### W5500 Ethernet SPI Pinout (Waveshare ESP32-S3-ETH)

| Signal | GPIO |
| :--- | :--- |
| **SPI MOSI** | `GPIO 11` |
| **SPI MISO** | `GPIO 12` |
| **SPI SCLK** | `GPIO 13` |
| **W5500 CS** | `GPIO 14` |
| **W5500 INT** | `GPIO 10` |
| **W5500 RST** | `GPIO 9` |

---

## Arduino IDE Setup & Board Configuration

### 1. Board Manager
Install **esp32 by Espressif Systems** (`v3.0.0` or newer is required for W5500 `ETH.begin` and `NetworkClient` support).

### 2. Required Libraries
Install the following via **Sketch > Include Library > Manage Libraries**:
* **PubSubClient** by Nick O'Leary (`v2.8+`)
* **ArduinoJson** by Benoit Blanchon (`v7.0+`)
* **NimBLE-Arduino** by h2zero (`v2.1.0+`)

#### Manual Installation: `opendroneid-core-c`
1. Download the ZIP archive from the `opendroneid/opendroneid-core-c` repository.
2. Extract the archive and copy **only** the `libopendroneid` subfolder into your `Arduino/libraries/` directory.
3. **Important:** Delete `wifi.c` from `Arduino/libraries/libopendroneid/` (it contains Linux-specific headers that will fail to compile on ESP32).
4. *(Optional)* Add a `library.properties` file inside `libopendroneid/` so the Arduino IDE indexes it cleanly.

### 3. Tools Menu Settings
Select **ESP32S3 Dev Module** under **Tools > Board**, and apply these settings:

| Setting | Value |
| :--- | :--- |
| **USB CDC On Boot** | `Enabled` |
| **CPU Frequency** | `240MHz (WiFi)` |
| **Core Debug Level** | `None` |
| **Events Run On** | `Core 0` *(or Core 1)* |
| **Arduino Runs On** | `Core 1` |
| **Flash Mode** | `QIO 80MHz` |
| **Flash Size** | `16MB (128Mb)` |
| **Partition Scheme** | `Huge APP (3MB No OTA/1MB SPIFFS)` |
| **PSRAM** | `OPI PSRAM` |
| **Upload Mode** | `UART0 / Hardware CDC` |
| **Upload Speed** | `921600` |

> **Flashing Tip:** If the board does not enter upload mode automatically, hold **BOOT**, tap **RESET**, release **BOOT**, and click **Upload**. Unplug USB-C before deploying on PoE.

---

## Code Configuration

Before compiling and uploading, open the `.ino` sketch and update the MQTT credentials at the top of the file to match your broker:

```cpp
// ==========================================
// 1. HOME ASSISTANT MQTT CONFIGURATION
// ==========================================
const char* mqtt_server = "192.168.1.50";  // Your MQTT Broker / Home Assistant IP
const int   mqtt_port   = 1883;
const char* mqtt_user   = "mqtt_user";     // MQTT Username (or "" if anonymous)
const char* mqtt_pass   = "mqtt_pass";     // MQTT Password
const char* mqtt_topic  = "drones/remoteid";
```

---

## MQTT Payload & Topics

### Telemetry Topic (`drones/remoteid`)
Published at a rate limit of **1 message per second per unique `drone_id`**:
```json
{
  "source": "BLE",
  "rssi": -68,
  "drone_id": "1581F5FJD234500A1B2C",
  "ua_type": 2,
  "lat": 42.31245,
  "lon": -83.58912,
  "alt_geo": 124.5,
  "speed_h": 8.2,
  "heading": 185,
  "pilot_lat": 42.31190,
  "pilot_lon": -83.58880
}
```

### Scanner Availability (`drones/scanner/status`)
Uses MQTT Last Will and Testament (LWT) with retained payloads (`"online"` on connect, `"offline"` on unexpected disconnect).

---

## Home Assistant Integration

Add the following to your `configuration.yaml` to create a map-compatible `device_tracker` and an online status `binary_sensor`:

```yaml
mqtt:
  device_tracker:
    - name: "Last Detected Drone"
      unique_id: "esp32s3_last_detected_drone"
      json_attributes_topic: "drones/remoteid"
      json_attributes_template: >
        {% if value_json.lat is defined and value_json.lon is defined and value_json.lat != 0 %}
          {
            "latitude": {{ value_json.lat }},
            "longitude": {{ value_json.lon }},
            "gps_accuracy": 10,
            "drone_id": "{{ value_json.drone_id | default('Unknown') }}",
            "altitude_m": {{ value_json.alt_geo | default(0) }},
            "speed_ms": {{ value_json.speed_h | default(0) }},
            "heading": {{ value_json.heading | default(0) }},
            "pilot_lat": {{ value_json.pilot_lat | default(0) }},
            "pilot_lon": {{ value_json.pilot_lon | default(0) }},
            "source": "{{ value_json.source | default('N/A') }}",
            "rssi": {{ value_json.rssi | default(0) }}
          }
        {% else %}
          {
            "drone_id": "{{ value_json.drone_id | default('Unknown') }}",
            "source": "{{ value_json.source | default('N/A') }}",
            "rssi": {{ value_json.rssi | default(0) }}
          }
        {% endif %}
      icon: mdi:quadcopter

  binary_sensor:
    - name: "Drone Scanner Status"
      unique_id: "esp32s3_drone_scanner_status"
      state_topic: "drones/scanner/status"
      payload_on: "online"
      payload_off: "offline"
      device_class: connectivity
      icon: mdi:radar
```

### Known Limitations (Arduino IDE vs. PlatformIO)
Pre-compiled Espressif board packages in the Arduino IDE have `CONFIG_BT_NIMBLE_EXT_ADV` disabled by default. As a result, builds compiled in the Arduino IDE capture **Wi-Fi Beacon/NAN** and **BLE 4 (1M PHY)** Remote ID broadcasts. To capture **BLE 5 Extended Advertising / Coded PHY** broadcasts, compile the project in PlatformIO with `-DCONFIG_BT_NIMBLE_EXT_ADV=1`.
