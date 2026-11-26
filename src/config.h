#pragma once
#include <Arduino.h>

#include "platform.h"

#define FW_NAME "LoGiX Fixture"
#define FW_VERSION "1.7.0"
#define FW_MODEL "logixfixture"  // board identifier for apps (mDNS TXT "model"), LoGiX naming scheme
#define FW_MANUFACTURER "Logix"
#define API_VERSION 1            // bump on breaking API changes (mDNS TXT "api")
#define MDNS_SERVICE "logix"     // advertised as _logix._tcp
#define WS_PORT 81               // WebSocket push channel

static const uint16_t MAX_LEDS = 800;

// Persistent device settings (/config.json on LittleFS).
struct Config {
  String name;               // friendly name; also drives hostname + setup AP SSID
  String ssid;               // home WiFi; empty = stay in setup-AP mode
  String pass;
  String apPass;             // setup AP password; empty = open AP (8..63 chars otherwise)
  int8_t ledPin = DEFAULT_LED_PIN;        // LED data GPIO (board default in platform.h)
  uint16_t ledCount = DEFAULT_LED_COUNT;
  String colorOrder = "GRB"; // WS2812B = GRB
  int8_t irPin = DEFAULT_IR_PIN;          // IR receiver GPIO; -1 = disabled
  uint16_t maxMa = DEFAULT_MAX_MA;        // total current budget in mA; 0 = no limit
  String apiToken;           // empty = API open; otherwise required on every API call
  String fixture = "custom"; // fixture model: custom, string, snowflake91 (UI icon + LED-count preset)
};

// Light state (/state.json, saved a few seconds after the last change).
struct State {
  bool on = true;
  uint8_t bri = 128;
  uint8_t r = 255, g = 147, b = 41;  // warm white
  uint8_t effect = 0;
  uint8_t speed = 128;
};

extern Config cfg;
extern State state;
extern uint32_t stateVersion;  // bumped on every state change (drives WebSocket pushes)

void configBegin();          // mount LittleFS and load config + state
bool configSave();
void stateMarkDirty();
void stateLoop();
void stateSaveNow();
void factoryReset();         // wipes settings, then reboots

void scheduleReboot(uint32_t delayMs);
void rebootLoop();

String defaultName();
String deviceId();           // stable ID = MAC lowercase without colons, e.g. "48e72971f314"
String setupApSsid();        // "LoGiX-Setup-F314"
String generateToken();      // 24 random hex chars from the hardware RNG
String deviceHostname();     // derived from cfg.name
String hostnameFromName(const String &name);

bool validLedPin(int pin);
bool validIrPin(int pin);
bool validColorOrder(const String &o);
bool validFixture(const String &f);
