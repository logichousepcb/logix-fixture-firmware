#pragma once
#include <Arduino.h>

#include "platform.h"

#define FW_NAME "LoGiX Fixture"
#define FW_VERSION "1.12.0"
#define FW_MODEL "logixfixture"  // board identifier for apps (mDNS TXT "model"), LoGiX naming scheme
#define FW_MANUFACTURER "Logix"
#define API_VERSION 1            // bump on breaking API changes (mDNS TXT "api")
#define MDNS_SERVICE "logix"     // advertised as _logix._tcp
#define WS_PORT 81               // WebSocket push channel

static const uint16_t MAX_LEDS = 800;

// One LED output (a strip on its own GPIO). The ESP8266 board has one, the WT32-ETH01 four.
struct Output {
  int8_t pin = -1;                        // LED data GPIO; -1 = output not used
  uint16_t count = DEFAULT_LED_COUNT;
  String order = "GRB";                   // WS2812B = GRB
  String name;                            // shown in the UI; empty = "Output N"
  String model = "custom";                // fixture model on this output (UI icon + fixed LED count)
  bool active = true;                     // listed on the web UI's control card (false = hidden there)
};

// Persistent device settings (/config.json on LittleFS).
struct Config {
  String name;               // friendly name; also drives hostname + setup AP SSID
  String ssid;               // home WiFi; empty = stay in setup-AP mode (or Ethernet only)
  String pass;
  String apPass;             // setup AP password; empty = open AP (8..63 chars otherwise)
  Output out[MAX_OUTPUTS];
  int8_t irPin = DEFAULT_IR_PIN;          // IR receiver GPIO; -1 = disabled
  uint16_t maxMa = DEFAULT_MAX_MA;        // total current budget in mA; 0 = no limit
  String apiToken;           // empty = API open; otherwise required on every API call
  Config() {
    for (uint8_t k = 0; k < MAX_OUTPUTS; k++) out[k].pin = DEFAULT_OUT_PINS[k];
  }
};

// What one output shows.
struct Seg {
  bool on = true;
  uint8_t bri = 255;                 // scaled by the master brightness
  uint8_t r = 255, g = 147, b = 41;  // color 1: warm white
  uint8_t c2[3] = {255, 0, 0};       // color 2: red
  uint8_t c3[3] = {0, 64, 255};      // color 3: blue
  uint8_t effect = 0;
  uint8_t speed = 128;
};

// Light state (/state.json, saved a few seconds after the last change).
struct State {
  bool on = true;     // master power
  uint8_t bri = 128;  // master brightness
  Seg seg[MAX_OUTPUTS];
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

bool outputUsed(uint8_t k);  // output k has a GPIO and LEDs
uint8_t firstOutput();       // lowest used output (0 if none)
uint16_t totalLeds();        // LEDs across all used outputs
String outputName(uint8_t k);

bool validLedPin(int pin);
bool validIrPin(int pin);
bool validColorOrder(const String &o);
bool validModel(const String &m);       // custom, string, snowflake91, arches4x24, tree81
uint16_t modelFixedCount(const String &m);  // LEDs a model always has; 0 = any count
String fixtureModel();       // model of the first used output (the device's "fixture" in the API)
