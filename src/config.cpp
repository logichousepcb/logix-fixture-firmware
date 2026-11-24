#include "config.h"

#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>

Config cfg;
State state;
uint32_t stateVersion = 0;

static const char *CFG_PATH = "/config.json";
static const char *STATE_PATH = "/state.json";
static const uint32_t STATE_SAVE_DELAY_MS = 5000;

static bool stateDirty = false;
static uint32_t stateDirtyAt = 0;
static bool rebootPending = false;
static uint32_t rebootAt = 0;

static String chipSuffix() {
  char b[5];
  snprintf(b, sizeof(b), "%04X", (unsigned)(ESP.getChipId() & 0xFFFF));
  return String(b);
}

String defaultName() { return String("LoGiX Fixture ") + chipSuffix(); }

// Full station MAC, lowercase, no separators (e.g. "48e72971f314") - same scheme as the
// other LoGiX devices, so an app can key every device the same way.
String deviceId() {
  uint8_t m[6];
  WiFi.macAddress(m);
  char b[13];
  snprintf(b, sizeof(b), "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

String setupApSsid() { return String("LoGiX-Setup-") + chipSuffix(); }

String generateToken() {
  String t;
  for (int i = 0; i < 3; i++) {
    char b[9];
    snprintf(b, sizeof(b), "%08x", (unsigned)RANDOM_REG32);  // hardware RNG (true random with RF on)
    t += b;
  }
  return t;
}

String hostnameFromName(const String &name) {
  String h;
  bool lastDash = false;
  for (unsigned i = 0; i < name.length() && h.length() < 31; i++) {
    char c = tolower(name[i]);
    if (isalnum((unsigned char)c)) {
      h += c;
      lastDash = false;
    } else if (!lastDash && h.length()) {
      h += '-';
      lastDash = true;
    }
  }
  while (h.endsWith("-")) h.remove(h.length() - 1);
  if (!h.length()) {
    h = "logix-fixture-" + chipSuffix();
    h.toLowerCase();
  }
  return h;
}

String deviceHostname() { return hostnameFromName(cfg.name); }

bool validLedPin(int p) {
  // 6-11 are the flash bus; GPIO16 can't be driven by the LED driver.
  return p == 0 || p == 1 || p == 2 || p == 3 || p == 4 || p == 5 || (p >= 12 && p <= 15);
}

bool validIrPin(int p) {
  // Needs an interrupt-capable pin (not 16). GPIO15 excluded: TSOP idles HIGH, GPIO15 must be LOW at boot.
  return p == -1 || (p >= 0 && p <= 5) || (p >= 12 && p <= 14);
}

bool validColorOrder(const String &o) {
  return o == "RGB" || o == "RBG" || o == "GRB" || o == "GBR" || o == "BRG" || o == "BGR";
}

bool validFixture(const String &f) { return f == "custom" || f == "string" || f == "snowflake91"; }

static void loadConfig() {
  cfg = Config();
  cfg.name = defaultName();
  File f = LittleFS.open(CFG_PATH, "r");
  if (!f) return;
  JsonDocument d;
  DeserializationError err = deserializeJson(d, f);
  f.close();
  if (err) {
    Serial.printf("[cfg] config.json unreadable (%s), using defaults\n", err.c_str());
    return;
  }
  if (d["name"].is<const char *>() && strlen(d["name"]) > 0) cfg.name = d["name"].as<const char *>();
  if (d["ssid"].is<const char *>()) cfg.ssid = d["ssid"].as<const char *>();
  if (d["pass"].is<const char *>()) cfg.pass = d["pass"].as<const char *>();
  if (d["apPass"].is<const char *>()) cfg.apPass = d["apPass"].as<const char *>();
  if (d["ledPin"].is<int>() && validLedPin(d["ledPin"])) cfg.ledPin = d["ledPin"];
  if (d["ledCount"].is<int>()) cfg.ledCount = constrain(d["ledCount"].as<int>(), 1, MAX_LEDS);
  if (d["colorOrder"].is<const char *>() && validColorOrder(d["colorOrder"].as<String>()))
    cfg.colorOrder = d["colorOrder"].as<const char *>();
  if (d["irPin"].is<int>() && validIrPin(d["irPin"])) cfg.irPin = d["irPin"];
  if (d["maxMa"].is<int>()) cfg.maxMa = d["maxMa"];
  if (d["apiToken"].is<const char *>()) cfg.apiToken = d["apiToken"].as<const char *>();
  if (d["fixture"].is<const char *>() && validFixture(d["fixture"].as<String>()))
    cfg.fixture = d["fixture"].as<const char *>();
  if (cfg.irPin == cfg.ledPin) cfg.irPin = -1;
}

bool configSave() {
  JsonDocument d;
  d["name"] = cfg.name;
  d["ssid"] = cfg.ssid;
  d["pass"] = cfg.pass;
  d["apPass"] = cfg.apPass;
  d["ledPin"] = cfg.ledPin;
  d["ledCount"] = cfg.ledCount;
  d["colorOrder"] = cfg.colorOrder;
  d["irPin"] = cfg.irPin;
  d["maxMa"] = cfg.maxMa;
  d["apiToken"] = cfg.apiToken;
  d["fixture"] = cfg.fixture;
  File f = LittleFS.open(CFG_PATH, "w");
  if (!f) return false;
  serializeJson(d, f);
  f.close();
  return true;
}

static void loadState() {
  File f = LittleFS.open(STATE_PATH, "r");
  if (!f) return;
  JsonDocument d;
  DeserializationError err = deserializeJson(d, f);
  f.close();
  if (err) return;
  state.on = d["on"] | state.on;
  state.bri = d["bri"] | state.bri;
  state.r = d["r"] | state.r;
  state.g = d["g"] | state.g;
  state.b = d["b"] | state.b;
  state.effect = d["effect"] | state.effect;
  state.speed = d["speed"] | state.speed;
}

void stateSaveNow() {
  JsonDocument d;
  d["on"] = state.on;
  d["bri"] = state.bri;
  d["r"] = state.r;
  d["g"] = state.g;
  d["b"] = state.b;
  d["effect"] = state.effect;
  d["speed"] = state.speed;
  File f = LittleFS.open(STATE_PATH, "w");
  if (f) {
    serializeJson(d, f);
    f.close();
  }
  stateDirty = false;
}

void stateMarkDirty() {
  stateDirty = true;
  stateDirtyAt = millis();
}

void stateLoop() {
  if (stateDirty && millis() - stateDirtyAt > STATE_SAVE_DELAY_MS) stateSaveNow();
}

void configBegin() {
  if (!LittleFS.begin()) {
    Serial.println("[cfg] LittleFS mount failed, formatting");
    LittleFS.format();
    LittleFS.begin();
  }
  loadConfig();
  loadState();
  Serial.printf("[cfg] name='%s' host=%s ssid='%s' led=GPIO%d x%u %s ir=GPIO%d limit=%umA\n",
                cfg.name.c_str(), deviceHostname().c_str(), cfg.ssid.c_str(), cfg.ledPin,
                cfg.ledCount, cfg.colorOrder.c_str(), cfg.irPin, cfg.maxMa);
}

void factoryReset() {
  LittleFS.remove(CFG_PATH);
  LittleFS.remove(STATE_PATH);
  stateDirty = false;
  rebootPending = true;
  rebootAt = millis() + 1000;
}

void scheduleReboot(uint32_t delayMs) {
  rebootPending = true;
  rebootAt = millis() + delayMs;
}

void rebootLoop() {
  if (!rebootPending || (int32_t)(millis() - rebootAt) < 0) return;
  if (stateDirty) stateSaveNow();
  Serial.println("[sys] rebooting");
  delay(100);
  ESP.restart();
}
