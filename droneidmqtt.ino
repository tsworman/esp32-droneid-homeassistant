#include <Arduino.h>
#include <SPI.h>
#include <ETH.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <NimBLEDevice.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

extern "C" {
  #include "opendroneid.h"
}

// ==========================================
// 1. HOME ASSISTANT MQTT CONFIGURATION
// ==========================================
const char* mqtt_server = "192.168.0.60";  // Replace with your Home Assistant IP
const int   mqtt_port   = 1883;
const char* mqtt_user   = "mqtt_user";     // Replace with HA MQTT username (or "" if none)
const char* mqtt_pass   = "mqtt_pass";     // Replace with HA MQTT password
const char* mqtt_topic  = "drones/remoteid";

// ==========================================
// 2. WAVESHARE ESP32-S3-ETH W5500 PINS
// ==========================================
#define ETH_SPI_SCK   13
#define ETH_SPI_MISO  12
#define ETH_SPI_MOSI  11
#define ETH_PHY_CS    14
#define ETH_PHY_IRQ   10
#define ETH_PHY_RST   9
#define ETH_PHY_ADDR  1

NetworkClient ethClient;
PubSubClient mqttClient(ethClient);

static bool eth_connected = false;
uint8_t current_channel = 1;
unsigned long last_hop = 0;

// Shared buffer & lock for Wi-Fi and BLE callbacks
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool has_new_drone = false;
ODID_UAS_Data latest_uas_data;
char latest_source[10] = "UNKNOWN";
int8_t latest_rssi = 0;

// Store parsed ODID data safely from either Wi-Fi or BLE task
void queueDroneData(const ODID_UAS_Data* uasData, const char* source, int8_t rssi) {
  portENTER_CRITICAL(&dataMux);
  memcpy(&latest_uas_data, uasData, sizeof(ODID_UAS_Data));
  strncpy(latest_source, source, sizeof(latest_source) - 1);
  latest_source[sizeof(latest_source) - 1] = '\0';
  latest_rssi = rssi;
  has_new_drone = true;
  portEXIT_CRITICAL(&dataMux);
}

// ==========================================
// 3. ETHERNET EVENT HANDLER
// ==========================================
void onEthEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      ETH.setHostname("esp32-s3-drone-scanner");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH Link Up");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.print("ETH IP: ");
      Serial.println(ETH.localIP());
      eth_connected = true;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH Disconnected");
      eth_connected = false;
      break;
    default:
      break;
  }
}

// ==========================================
// 4. WI-FI PROMISCUOUS REMOTE ID SCANNER
// ==========================================
void wifi_sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;

  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  const uint8_t* payload = pkt->payload;
  uint16_t len = pkt->rx_ctrl.sig_len;

  if (len < 42) return;

  for (uint16_t i = 24; i < len - 6; i++) {
    // ASTM F3411 Vendor Specific IE (0xDD) + OUI (FA:0B:BC) + Type (0x0D)
    if (payload[i] == 0xDD && payload[i + 2] == 0xFA && 
        payload[i + 3] == 0x0B && payload[i + 4] == 0xBC && payload[i + 5] == 0x0D) {
      
      uint8_t ie_len = payload[i + 1];
      if (i + 2 + ie_len <= len && ie_len > 5) {
        const uint8_t* odid_payload = &payload[i + 7];
        uint16_t odid_len = ie_len - 5;

        ODID_UAS_Data uasData;
        odid_initUasData(&uasData);
        
        uint8_t msgType = (odid_payload[0] >> 4) & 0x0F;
        if (msgType == ODID_MESSAGETYPE_PACKED) {
          decodeMessagePack(&uasData, (const ODID_MessagePack_encoded*)odid_payload);
        } else if (odid_len >= 25) {
          decodeOpenDroneID(&uasData, (uint8_t*)odid_payload);
        }
        
        if (uasData.BasicIDValid[0] || uasData.LocationValid) {
          queueDroneData(&uasData, "WIFI", pkt->rx_ctrl.rssi);
        }
      }
      break;
    }
  }
}

// ==========================================
// 5. BLE REMOTE ID SCANNER
// ==========================================
volatile uint32_t ble_pkt_count = 0;

class DroneBLECallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    ble_pkt_count++;

    if (!advertisedDevice->haveServiceData()) return;

    // Check if 0xFFFA Service Data exists
    std::string serviceData = advertisedDevice->getServiceData(NimBLEUUID((uint16_t)0xFFFA));
    if (serviceData.empty()) return;

    //Serial.printf("0xFFFA Packet! Len=%d, RSSI=%d\n", serviceData.length(), advertisedDevice->getRSSI());

    if (serviceData.length() < 27) return;

    const uint8_t* data = (const uint8_t*)serviceData.data();
    size_t len = serviceData.length();

    if (*data != 0x0D) {
      Serial.printf("Unexpected App Code: 0x%02X\n", *data);
      return;
    }

    const uint8_t* msg = data + 2;
    size_t msg_len = len - 2;

    ODID_UAS_Data uasData;
    odid_initUasData(&uasData);

    uint8_t msgType = (*msg >> 4) & 0x0F;
    if (msgType == ODID_MESSAGETYPE_PACKED) {
      decodeMessagePack(&uasData, (const ODID_MessagePack_encoded*)msg);
    } else if (msg_len >= 25) {
      decodeOpenDroneID(&uasData, (uint8_t*)msg);
    }

    if (uasData.BasicIDValid || uasData.LocationValid) {
      Serial.println("BLE Drone Packet Decoded!");
      queueDroneData(&uasData, "BLE", advertisedDevice->getRSSI());
    }
  }
};


// ==========================================
// 6. MQTT CONNECTION & PUBLISHING
// ==========================================
void reconnectMQTT() {
  if (!eth_connected) return;
  if (!mqttClient.connected()) {
    Serial.print("Connecting to HA MQTT...");
    // Arguments: clientID, user, pass, willTopic, willQoS, willRetain, willMessage
    if (mqttClient.connect("ESP32S3_DroneScanner", mqtt_user, mqtt_pass, "drones/scanner/status", 1, true, "offline")) {
      Serial.println("connected!");
      mqttClient.publish("drones/scanner/status", "online", true);
    } else {
      Serial.print("failed, rc=");
      Serial.println(mqttClient.state());
    }
  }
}

// ==========================================
// RATE LIMITER (Max 1 MQTT publish/sec per Drone ID)
// ==========================================
#define MAX_TRACKED_DRONES 10
#define PUBLISH_INTERVAL_MS 1000 // 1000ms = 1 second per drone

struct TrackedDrone {
  char id[21];
  unsigned long last_published;
};

TrackedDrone trackedDrones[MAX_TRACKED_DRONES];

bool shouldPublishDrone(const char* droneId) {
  unsigned long now = millis();
  int oldestIdx = 0;
  unsigned long oldestTime = ULONG_MAX;

  for (int i = 0; i < MAX_TRACKED_DRONES; i++) {
    // Match existing drone ID
    if (trackedDrones[i].id[0] != '\0' && strcmp(trackedDrones[i].id, droneId) == 0) {
      if (now - trackedDrones[i].last_published >= PUBLISH_INTERVAL_MS) {
        trackedDrones[i].last_published = now;
        return true;
      }
      return false; // Still within cooldown window
    }

    // Track empty or oldest slot in case this is a brand-new drone
    if (trackedDrones[i].id[0] == '\0') {
      oldestIdx = i;
      oldestTime = 0;
    } else if (trackedDrones[i].last_published < oldestTime) {
      oldestTime = trackedDrones[i].last_published;
      oldestIdx = i;
    }
  }

  // Register new drone in empty/oldest slot and allow immediate publish
  strncpy(trackedDrones[oldestIdx].id, droneId, sizeof(trackedDrones[oldestIdx].id) - 1);
  trackedDrones[oldestIdx].id[sizeof(trackedDrones[oldestIdx].id) - 1] = '\0';
  trackedDrones[oldestIdx].last_published = now;
  return true;
}

void publishDroneTelemetry() {
  ODID_UAS_Data uasCopy;
  char sourceCopy[10];
  int8_t rssiCopy;

  portENTER_CRITICAL(&dataMux);
  memcpy(&uasCopy, &latest_uas_data, sizeof(ODID_UAS_Data));
  strncpy(sourceCopy, latest_source, sizeof(sourceCopy));
  rssiCopy = latest_rssi;
  has_new_drone = false;
  portEXIT_CRITICAL(&dataMux);

  // Extract ID (fallback to "UNKNOWN" if a Location-only packet arrives first)
  const char* droneId = uasCopy.BasicIDValid[0] ? (const char*)uasCopy.BasicID[0].UASID : "UNKNOWN";

  // Skip publishing if we already sent an update for this drone within PUBLISH_INTERVAL_MS
  if (!shouldPublishDrone(droneId)) {
    return;
  }

  JsonDocument doc;
  doc["source"] = sourceCopy;
  doc["rssi"]   = rssiCopy;

  if (uasCopy.BasicIDValid[0]) {
    doc["drone_id"] = (char*)uasCopy.BasicID[0].UASID;
    doc["ua_type"]  = uasCopy.BasicID[0].UAType;
  }

  if (uasCopy.LocationValid) {
    doc["lat"]      = uasCopy.Location.Latitude;
    doc["lon"]      = uasCopy.Location.Longitude;
    doc["alt_geo"]  = uasCopy.Location.AltitudeGeo;
    doc["speed_h"]  = uasCopy.Location.SpeedHorizontal;
    doc["heading"]  = uasCopy.Location.Direction;
  }

  if (uasCopy.SystemValid) {
    doc["pilot_lat"] = uasCopy.System.OperatorLatitude;
    doc["pilot_lon"] = uasCopy.System.OperatorLongitude;
  }

  char jsonBuffer[384];
  serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));

  if (mqttClient.connected()) {
    mqttClient.publish(mqtt_topic, jsonBuffer);
    Serial.print("Published: ");
    Serial.println(jsonBuffer);
  }
}

// ==========================================
// 7. SETUP & MAIN LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize W5500 Ethernet
  Network.onEvent(onEthEvent);
  SPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI);
  ETH.begin(ETH_PHY_W5500, ETH_PHY_ADDR, ETH_PHY_CS, ETH_PHY_IRQ, ETH_PHY_RST, SPI);
  mqttClient.setServer(mqtt_server, mqtt_port);

  // Initialize Wi-Fi Promiscuous Sniffer
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_cb);
  esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);

  // Initialize NimBLE (Standard 1M PHY for Arduino IDE compatibility)
  NimBLEDevice::init("");
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(new DroneBLECallbacks(), false);
  pScan->setActiveScan(false);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->start(0, false);

  Serial.println("Wi-Fi + BLE Remote ID Scanner & W5500 MQTT Relay Active");
}

void loop() {
  static unsigned long last_mqtt_attempt = 0;

  if (eth_connected) {
    if (!mqttClient.connected() && millis() - last_mqtt_attempt > 5000) {
      last_mqtt_attempt = millis();
      reconnectMQTT();
    }
    mqttClient.loop();
  }

  //static unsigned long last_stat = 0;
  //if (millis() - last_stat > 5000) {
  //  last_stat = millis();
  //  Serial.printf("Heartbeat - BLE packets seen: %lu\n", ble_pkt_count);
  //  ble_pkt_count = 0;
  //}

  // Hop Wi-Fi channels 1-11 every 250ms
  if (millis() - last_hop > 250) {
    current_channel = (current_channel % 11) + 1;
    esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
    last_hop = millis();
  }

  // Publish telemetry when detected
  if (has_new_drone) {
    publishDroneTelemetry();
  }
}
