#pragma once
#include <stdint.h>

// Connection state of the network stack.
//
// Lives in its own Arduino-free header so host (native) code — the controller's
// ISnapshotObserver in particular — can name it without pulling in WiFi.h /
// PubSubClient. NetManager.h includes this and owns the state machine itself.
//
// The device is standalone-first: BLE and local control must keep running while
// WiFi/MQTT are down, so nothing here ever blocks for long and nothing ever
// restarts the ESP32. Transitions are:
//
//   WIFI_DOWN ──(backoff elapsed, WiFi.begin())──▶ WIFI_CONNECTING
//   WIFI_CONNECTING ──(WL_CONNECTED)────────────▶ WIFI_UP        [OTA+mDNS start once]
//   WIFI_CONNECTING ──(timeout)─────────────────▶ WIFI_DOWN      [backoff++]
//   WIFI_UP ──(backoff elapsed)─────────────────▶ MQTT_CONNECTING
//   MQTT_CONNECTING ──(connect ok)──────────────▶ MQTT_UP        [backoff reset, onMqttConnected()]
//   MQTT_CONNECTING ──(connect failed)──────────▶ WIFI_UP        [backoff++]
//   MQTT_UP ──(broker dropped us)───────────────▶ WIFI_UP        [retry after first backoff step]
//   any of WIFI_UP/MQTT_*  ──(WiFi lost)────────▶ WIFI_DOWN      [backoff reset, retry now]
enum class NetStatus : uint8_t
{
    WIFI_DOWN,
    WIFI_CONNECTING,
    WIFI_UP,
    MQTT_CONNECTING,
    MQTT_UP,
};
