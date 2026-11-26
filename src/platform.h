#pragma once
// Board / chip abstraction. Everything that differs between the ESP8266 fixture board and the
// ESP32 boards lives here, so the rest of the firmware stays the same for both.
//
//   esp12       ESP-12F (ESP8266) fixture board: WiFi only, LEDs on GPIO2, IR on GPIO4
//   wt32-eth01  WT32-ETH01 (ESP32 + LAN8720): Ethernet first, WiFi as backup

#include <Arduino.h>

#if defined(ESP32)
#include <ETH.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>

#define BOARD_ID "wt32-eth01"
#define HAS_ETH 1
using WebServerT = WebServer;

// WT32-ETH01 Ethernet wiring (LAN8720). These pins are reserved and can't be used for LEDs/IR.
static const uint8_t ETH01_PHY_ADDR = 1;
static const int ETH01_PHY_POWER = 16;  // enables the 50 MHz oscillator
static const int ETH01_MDC = 23;
static const int ETH01_MDIO = 18;
// RMII data pins 19, 21, 22, 25, 26, 27 and the clock input on GPIO0 are also taken.

// Defaults for a fresh board. GPIO4 is one of the 4 LED outputs and keeps serial logging working.
static const int8_t DEFAULT_LED_PIN = 4;
static const uint16_t DEFAULT_LED_COUNT = 125;
static const int8_t DEFAULT_IR_PIN = -1;
static const uint16_t DEFAULT_MAX_MA = 0;  // powered from an external LED supply: no limit
static const uint16_t BOARD_MA = 200;      // ESP32 + Ethernet PHY overhead

static inline uint32_t chipId24() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  return ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 8) | m[5];
}
static inline void stationMac(uint8_t *m) { esp_read_mac(m, ESP_MAC_WIFI_STA); }
static inline uint32_t hwRandom() { return esp_random(); }
static inline uint32_t flashSize() { return ESP.getFlashChipSize(); }
static inline String updateErrorStr() { return String(Update.errorString()); }
static inline bool updateBegin() { return Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH); }
static inline String resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "Power on";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software restart";
    case ESP_RST_PANIC: return "Crash (panic)";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_BROWNOUT: return "Brownout";
    default: return "Unknown";
  }
}
// First bytes of an uploaded firmware image: must be an ESP32 (chip id 0) app image.
static inline bool firmwareImageOk(const uint8_t *b, size_t n) {
  return n >= 16 && b[0] == 0xE9 && b[12] == 0x00 && b[13] == 0x00;
}

#else  // ESP8266
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>

#define BOARD_ID "esp12"
#define HAS_ETH 0
using WebServerT = ESP8266WebServer;

static const int8_t DEFAULT_LED_PIN = 2;  // board: GPIO2 -> headers U2/U3/U4
static const uint16_t DEFAULT_LED_COUNT = 30;
static const int8_t DEFAULT_IR_PIN = 4;   // board: TSOP38238 on GPIO4
static const uint16_t DEFAULT_MAX_MA = 500;
static const uint16_t BOARD_MA = 80;      // ESP8266 + regulator overhead

static inline uint32_t chipId24() { return ESP.getChipId(); }
static inline void stationMac(uint8_t *m) { WiFi.macAddress(m); }
static inline uint32_t hwRandom() { return RANDOM_REG32; }  // true random with RF on
static inline uint32_t flashSize() { return ESP.getFlashChipRealSize(); }
static inline String updateErrorStr() { return Update.getErrorString(); }
static inline bool updateBegin() {
  uint32_t space = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
  return Update.begin(space, U_FLASH);
}
static inline String resetReason() { return ESP.getResetReason(); }
// ESP32 images carry an extended header whose first byte (offset 8, WP pin) is 0xEE; ESP8266
// images have a segment address there. Refuse ESP32 files so this board isn't bricked.
static inline bool firmwareImageOk(const uint8_t *b, size_t n) { return n >= 16 && b[0] == 0xE9 && b[8] != 0xEE; }
#endif
