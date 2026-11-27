#include "config.h"

#include <ArduinoJson.h>

#include "leds.h"

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
  snprintf(b, sizeof(b), "%04X", (unsigned)(chipId24() & 0xFFFF));
  return String(b);
}

String defaultName() { return String("LoGiX Fixture ") + chipSuffix(); }

// Full station MAC, lowercase, no separators (e.g. "48e72971f314") - same scheme as the
// other LoGiX devices, so an app can key every device the same way.
String deviceId() {
  uint8_t m[6];
  stationMac(m);
  char b[13];
  snprintf(b, sizeof(b), "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

String setupApSsid() { return String("LoGiX-Setup-") + chipSuffix(); }

String generateToken() {
  String t;
  for (int i = 0; i < 3; i++) {
    char b[9];
    snprintf(b, sizeof(b), "%08x", (unsigned)hwRandom());  // hardware RNG
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

#if defined(ESP32)
// WT32-ETH01: GPIO0, 16, 18, 19, 21, 22, 23, 25, 26, 27 belong to Ethernet; 6-11 are the flash bus.
// 34-39 are input-only, so they can take an IR receiver but not LEDs.
bool validLedPin(int p) {
  return p == 1 || p == 2 || p == 3 || p == 4 || p == 5 || p == 12 || p == 14 || p == 15 || p == 17 ||
         p == 32 || p == 33;
}

bool validIrPin(int p) { return p == -1 || validLedPin(p) || p == 35 || p == 36 || p == 39; }
#else
bool validLedPin(int p) {
  // 6-11 are the flash bus; GPIO16 can't be driven by the LED driver.
  return p == 0 || p == 1 || p == 2 || p == 3 || p == 4 || p == 5 || (p >= 12 && p <= 15);
}

bool validIrPin(int p) {
  // Needs an interrupt-capable pin (not 16). GPIO15 excluded: TSOP idles HIGH, GPIO15 must be LOW at boot.
  return p == -1 || (p >= 0 && p <= 5) || (p >= 12 && p <= 14);
}
#endif

bool outputUsed(uint8_t k) { return k < MAX_OUTPUTS && cfg.out[k].pin >= 0 && cfg.out[k].count > 0; }

uint8_t firstOutput() {
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++)
    if (outputUsed(k)) return k;
  return 0;
}

uint16_t totalLeds() {
  uint16_t t = 0;
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++)
    if (outputUsed(k)) t += cfg.out[k].count;
  return t;
}

String outputName(uint8_t k) { return cfg.out[k].name.length() ? cfg.out[k].name : String("Output ") + (k + 1); }

bool validColorOrder(const String &o) {
  return o == "RGB" || o == "RBG" || o == "GRB" || o == "GBR" || o == "BRG" || o == "BGR";
}

bool validModel(const String &m) {
  return m == "custom" || m == "string" || m == "snowflake91" || m == "arches4x24" || m == "tree81";
}

uint16_t modelFixedCount(const String &m) {
  if (m == "snowflake91") return 91;
  if (m == "arches4x24") return 96;  // 4 arches x 24 pixels
  if (m == "tree81") return 81;
  return 0;
}

String fixtureModel() { return cfg.out[firstOutput()].model; }

static void loadConfig() {
  cfg = Config();
  cfg.name = defaultName();
  if (!LittleFS.exists(CFG_PATH)) return;
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
  JsonArrayConst outs = d["outputs"];
  if (outs) {
    for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
      Output &o = cfg.out[k];
      JsonObjectConst j = outs[k];
      if (!j) {
        o.pin = -1;
        continue;
      }
      o.pin = j["pin"].is<int>() && validLedPin(j["pin"]) ? j["pin"].as<int>() : -1;
      if (j["count"].is<int>()) o.count = constrain(j["count"].as<int>(), 1, MAX_LEDS);
      if (j["order"].is<const char *>() && validColorOrder(j["order"].as<String>())) o.order = j["order"].as<const char *>();
      if (j["name"].is<const char *>()) o.name = j["name"].as<const char *>();
      if (j["model"].is<const char *>() && validModel(j["model"].as<String>())) o.model = j["model"].as<const char *>();
      o.active = j["active"] | true;
    }
  } else {  // config from before multiple outputs: one strip
    for (uint8_t k = 1; k < MAX_OUTPUTS; k++) cfg.out[k].pin = -1;
    Output &o = cfg.out[0];
    if (d["ledPin"].is<int>() && validLedPin(d["ledPin"])) o.pin = d["ledPin"];
    if (d["ledCount"].is<int>()) o.count = constrain(d["ledCount"].as<int>(), 1, MAX_LEDS);
    if (d["colorOrder"].is<const char *>() && validColorOrder(d["colorOrder"].as<String>()))
      o.order = d["colorOrder"].as<const char *>();
  }
  if (d["irPin"].is<int>() && validIrPin(d["irPin"])) cfg.irPin = d["irPin"];
  if (d["maxMa"].is<int>()) cfg.maxMa = d["maxMa"];
  if (d["apiToken"].is<const char *>()) cfg.apiToken = d["apiToken"].as<const char *>();
  // before per-output models (1.9.0) there was one device-wide model; it belonged to output 1
  if (!outs[0]["model"].is<const char *>() && d["fixture"].is<const char *>() &&
      validModel(d["fixture"].as<String>()))
    cfg.out[0].model = d["fixture"].as<const char *>();
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++)
    if (uint16_t n = modelFixedCount(cfg.out[k].model)) cfg.out[k].count = n;
  // Drop outputs that clash with an earlier one or don't fit in MAX_LEDS; IR loses to LEDs.
  uint16_t total = 0;
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    if (!outputUsed(k)) continue;
    for (uint8_t j = 0; j < k; j++)
      if (outputUsed(j) && cfg.out[j].pin == cfg.out[k].pin) cfg.out[k].pin = -1;
    if (outputUsed(k) && total + cfg.out[k].count > MAX_LEDS) cfg.out[k].pin = -1;
    if (outputUsed(k)) total += cfg.out[k].count;
    if (outputUsed(k) && cfg.irPin == cfg.out[k].pin) cfg.irPin = -1;
  }
}

bool configSave() {
  JsonDocument d;
  d["name"] = cfg.name;
  d["ssid"] = cfg.ssid;
  d["pass"] = cfg.pass;
  d["apPass"] = cfg.apPass;
  JsonArray outs = d["outputs"].to<JsonArray>();
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    JsonObject o = outs.add<JsonObject>();
    o["pin"] = cfg.out[k].pin;
    o["count"] = cfg.out[k].count;
    o["order"] = cfg.out[k].order;
    o["name"] = cfg.out[k].name;
    o["model"] = cfg.out[k].model;
    o["active"] = cfg.out[k].active;
  }
  // first output also in the old single-strip fields, so older firmware still reads it
  d["ledPin"] = cfg.out[0].pin >= 0 ? cfg.out[0].pin : DEFAULT_OUT_PINS[0];
  d["ledCount"] = cfg.out[0].count;
  d["colorOrder"] = cfg.out[0].order;
  d["irPin"] = cfg.irPin;
  d["maxMa"] = cfg.maxMa;
  d["apiToken"] = cfg.apiToken;
  d["fixture"] = fixtureModel();  // read by firmware before 1.9.0
  File f = LittleFS.open(CFG_PATH, "w");
  if (!f) return false;
  serializeJson(d, f);
  f.close();
  return true;
}

static void loadState() {
  if (!LittleFS.exists(STATE_PATH)) return;
  File f = LittleFS.open(STATE_PATH, "r");
  if (!f) return;
  JsonDocument d;
  DeserializationError err = deserializeJson(d, f);
  f.close();
  if (err) return;
  state.on = d["on"] | state.on;
  state.bri = d["bri"] | state.bri;
  JsonArrayConst segs = d["seg"];
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    Seg &s = state.seg[k];
    // per-output state; a file from before multiple outputs has one set of fields for all
    JsonObjectConst j = segs ? segs[k].as<JsonObjectConst>() : d.as<JsonObjectConst>();
    if (!j) continue;
    if (segs) {  // in the old format "on"/"bri" are the master values, read above
      s.on = j["on"] | s.on;
      s.bri = j["bri"] | s.bri;
    }
    s.r = j["r"] | s.r;
    s.g = j["g"] | s.g;
    s.b = j["b"] | s.b;
    for (uint8_t i = 0; i < 3; i++) {
      s.c2[i] = j["c2"][i] | s.c2[i];
      s.c3[i] = j["c3"][i] | s.c3[i];
    }
    s.effect = j["effect"] | s.effect;
    if (s.effect >= EFFECT_COUNT) s.effect = 0;
    s.speed = j["speed"] | s.speed;
  }
}

void stateSaveNow() {
  JsonDocument d;
  d["on"] = state.on;
  d["bri"] = state.bri;
  JsonArray segs = d["seg"].to<JsonArray>();
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    const Seg &s = state.seg[k];
    JsonObject j = segs.add<JsonObject>();
    j["on"] = s.on;
    j["bri"] = s.bri;
    j["r"] = s.r;
    j["g"] = s.g;
    j["b"] = s.b;
    JsonArray c2 = j["c2"].to<JsonArray>(), c3 = j["c3"].to<JsonArray>();
    for (uint8_t i = 0; i < 3; i++) {
      c2.add(s.c2[i]);
      c3.add(s.c3[i]);
    }
    j["effect"] = s.effect;
    j["speed"] = s.speed;
  }
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
  Serial.printf("[cfg] name='%s' host=%s ssid='%s' ir=GPIO%d limit=%umA\n", cfg.name.c_str(),
                deviceHostname().c_str(), cfg.ssid.c_str(), cfg.irPin, cfg.maxMa);
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++)
    if (outputUsed(k))
      Serial.printf("[cfg] output %u '%s': GPIO%d x%u %s\n", k + 1, outputName(k).c_str(), cfg.out[k].pin,
                    cfg.out[k].count, cfg.out[k].order.c_str());
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
