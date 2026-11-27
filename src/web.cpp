#include "web.h"

#include <ArduinoJson.h>
#include <WebSocketsServer.h>

#include "config.h"
#include "icons.h"
#include "ir.h"
#include "leds.h"
#include "net.h"
#include "platform.h"
#include "web_ui.h"

#if defined(ESP32)
#include <esp_partition.h>
#endif

static WebServerT server(80);
static WebSocketsServer ws(WS_PORT);
static String updateError;
static bool updateFirstChunk = false;
static uint32_t wsSentVersion = 0;
static uint32_t wsLastPush = 0;

// --------------------------------------------------------------------------- auth
// Optional API token (cfg.apiToken). When set, every /api call except a minimal
// GET /api/info must carry it as "Authorization: Bearer <token>", "X-Api-Key: <token>"
// or "?token=<token>". The WebSocket takes it as ws://host:81/?token=<token>.
static bool tokenEquals(const String &a, const String &b) {  // constant-time
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (unsigned i = 0; i < a.length(); i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

static String requestToken() {
  String a = server.header("Authorization");
  if (a.startsWith("Bearer ")) return a.substring(7);
  if (server.header("X-Api-Key").length()) return server.header("X-Api-Key");
  if (server.hasArg("token")) return server.arg("token");
  return "";
}

static bool authed() { return !cfg.apiToken.length() || tokenEquals(requestToken(), cfg.apiToken); }

// --------------------------------------------------------------------------- helpers
static void sendJson(int code, const JsonDocument &d) {
  String out;
  serializeJson(d, out);
  server.send(code, "application/json", out);
}

static void sendOk(bool reboot = false) {
  JsonDocument d;
  d["ok"] = true;
  if (reboot) d["reboot"] = true;
  sendJson(200, d);
}

static void sendErr(int code, const String &msg) {
  JsonDocument d;
  d["ok"] = false;
  d["error"] = msg;
  sendJson(code, d);
}

// Wraps a handler so it only runs with a valid token.
template <void (*H)()>
static void guarded() {
  if (authed()) return H();
  server.sendHeader("WWW-Authenticate", "Bearer");
  sendErr(401, "unauthorized: API token required");
}

static bool readBody(JsonDocument &d) {
  if (!server.hasArg("plain")) {
    sendErr(400, "missing JSON body");
    return false;
  }
  if (deserializeJson(d, server.arg("plain")) || !d.is<JsonObject>()) {
    sendErr(400, "body must be a JSON object");
    return false;
  }
  return true;
}

// Accepts a JSON number or a numeric string (so query-string args work too).
static bool getInt(JsonVariantConst v, long &out) {
  if (v.is<long>()) {
    out = v.as<long>();
    return true;
  }
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    char *end;
    long x = strtol(s, &end, 10);
    if (end != s && *end == 0) {
      out = x;
      return true;
    }
  }
  return false;
}

static bool parseHex(String s, uint8_t &r, uint8_t &g, uint8_t &b) {
  s.trim();
  if (s.startsWith("#")) s.remove(0, 1);
  if (s.length() != 6) return false;
  for (unsigned i = 0; i < 6; i++)
    if (!isxdigit((unsigned char)s[i])) return false;
  uint32_t v = strtoul(s.c_str(), nullptr, 16);
  r = v >> 16;
  g = v >> 8;
  b = v;
  return true;
}

static void colorToJson(JsonObject o, const Seg &s) {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02x%02x%02x", s.r, s.g, s.b);
  JsonArray c = o["color"].to<JsonArray>();
  c.add(s.r);
  c.add(s.g);
  c.add(s.b);
  o["hex"] = hex;
  // colors 1-3 ("color"/"hex" above = color 1)
  JsonArray cs = o["colors"].to<JsonArray>();
  cs.add(hex);
  for (const uint8_t *c : {s.c2, s.c3}) {
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", c[0], c[1], c[2]);
    cs.add(hex);
  }
  o["effect"] = s.effect;
  o["effectName"] = EFFECT_NAMES[s.effect];
  o["speed"] = s.speed;
}

// Top level: master on/bri, plus color/effect/speed of the first output (the whole fixture
// when all outputs match, which is what single-light apps see). "segments" has every output.
static void stateToJson(JsonObject o) {
  const uint8_t f = firstOutput();
  const Seg &a = state.seg[f];
  o["on"] = state.on;
  o["bri"] = state.bri;
  colorToJson(o, a);
  bool synced = true;
  JsonArray segs = o["segments"].to<JsonArray>();
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    if (!outputUsed(k)) continue;
    const Seg &s = state.seg[k];
    // same look: same effect, and the same color/speed where the effect uses them
    const uint8_t nc = effectColors(s.effect);
    bool sameColor = s.r == a.r && s.g == a.g && s.b == a.b && (nc < 2 || !memcmp(s.c2, a.c2, 3)) &&
                     (nc < 3 || !memcmp(s.c3, a.c3, 3));
    if (s.effect != a.effect || (nc && !sameColor) || (s.effect != 0 && s.speed != a.speed))
      synced = false;
    JsonObject j = segs.add<JsonObject>();
    j["id"] = k;
    j["name"] = outputName(k);
    j["model"] = cfg.out[k].model;
    j["active"] = cfg.out[k].active;  // false = hidden on the web UI's control card
    j["on"] = s.on;
    j["bri"] = s.bri;
    colorToJson(j, s);
  }
  o["synced"] = synced;  // every output shows the same color/effect/speed
}

static void sendState() {
  JsonDocument d;
  stateToJson(d.to<JsonObject>());
  sendJson(200, d);
}

// "on": true/false/"toggle" (also "1"/"0"/"on"/"off" from query strings).
static String parseOn(JsonVariantConst on, bool &val) {
  if (on.isNull()) return "";
  if (on.is<bool>()) {
    val = on.as<bool>();
    return "";
  }
  String t = on.as<String>();
  t.toLowerCase();
  if (t == "toggle" || t == "t") val = !val;
  else if (t == "1" || t == "true" || t == "on") val = true;
  else if (t == "0" || t == "false" || t == "off") val = false;
  else return "on must be true, false or \"toggle\"";
  return "";
}

// Reads color / r / g / b / effect / speed into s (and on / bri when withOnBri).
// One color: [r,g,b] or "#rrggbb".
static bool parseColor(JsonVariantConst col, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (col.is<JsonArrayConst>()) {
    if (col.size() < 3) return false;
    long c[3];
    for (int i = 0; i < 3; i++)
      if (!getInt(col[i], c[i]) || c[i] < 0 || c[i] > 255) return false;
    r = c[0];
    g = c[1];
    b = c[2];
    return true;
  }
  return parseHex(col.as<String>(), r, g, b);
}

static String parseSeg(JsonObjectConst o, Seg &s, bool withOnBri) {
  long v;
  if (withOnBri) {
    String err = parseOn(o["on"], s.on);
    if (err.length()) return err;
    if (!o["bri"].isNull()) {
      if (!getInt(o["bri"], v) || v < 0 || v > 255) return "bri must be 0-255";
      s.bri = v;
    }
  }
  JsonVariantConst col = o["color"];
  if (!col.isNull() && !parseColor(col, s.r, s.g, s.b)) return "color must be [r,g,b] (0-255) or \"#rrggbb\"";
  // colors: [color1, color2, color3], each [r,g,b] / "#rrggbb" / null (unchanged); may be shorter
  JsonVariantConst cols = o["colors"];
  if (!cols.isNull()) {
    if (!cols.is<JsonArrayConst>() || cols.size() > 3) return "colors must be an array of up to 3 colors";
    uint8_t *dst[3][3] = {{&s.r, &s.g, &s.b}, {&s.c2[0], &s.c2[1], &s.c2[2]}, {&s.c3[0], &s.c3[1], &s.c3[2]}};
    for (uint8_t i = 0; i < cols.size(); i++) {
      if (cols[i].isNull()) continue;
      if (!parseColor(cols[i], *dst[i][0], *dst[i][1], *dst[i][2]))
        return "colors[" + String(i) + "] must be [r,g,b] (0-255) or \"#rrggbb\"";
    }
  }
  const char *ch[3] = {"r", "g", "b"};
  uint8_t *dst[3] = {&s.r, &s.g, &s.b};
  for (int i = 0; i < 3; i++) {
    if (o[ch[i]].isNull()) continue;
    if (!getInt(o[ch[i]], v) || v < 0 || v > 255) return String(ch[i]) + " must be 0-255";
    *dst[i] = v;
  }
  JsonVariantConst fx = o["effect"];
  if (!fx.isNull()) {
    if (getInt(fx, v)) {
      if (v < 0 || v >= EFFECT_COUNT) return "unknown effect";
      s.effect = v;
    } else {
      String name = fx.as<String>();
      int found = -1;
      for (uint8_t i = 0; i < EFFECT_COUNT; i++)
        if (name.equalsIgnoreCase(EFFECT_NAMES[i])) found = i;
      if (found < 0) return "unknown effect";
      s.effect = found;
    }
  }
  if (!o["speed"].isNull()) {
    if (!getInt(o["speed"], v) || v < 0 || v > 255) return "speed must be 0-255";
    s.speed = v;
  }
  return "";
}

static String segIndex(JsonVariantConst v, uint8_t &k) {
  long x;
  if (!getInt(v, x) || x < 0 || x >= MAX_OUTPUTS || !outputUsed(x)) return "unknown output (seg / segments id)";
  k = x;
  return "";
}

// Validates everything first, then applies. Returns "" on success or an error message.
//   {on, bri}                         master power / brightness
//   {color, effect, speed, r, g, b}   every output (or only output "seg" if given)
//   {seg: 2, on, bri, ...}            one output's own on/bri/color/effect/speed
//   {segments: [{id: 2, ...}, ...]}   several outputs at once
static String applyState(JsonObjectConst o) {
  State ns = state;
  String err;
  bool syncAll = false;
  if (!o["seg"].isNull()) {
    uint8_t k;
    if ((err = segIndex(o["seg"], k)).length()) return err;
    if ((err = parseSeg(o, ns.seg[k], true)).length()) return err;
    if (ns.seg[k].on && !state.seg[k].on) ns.on = true;  // switching an output on wakes the fixture
  } else {
    if ((err = parseOn(o["on"], ns.on)).length()) return err;
    long v;
    if (!o["bri"].isNull()) {
      if (!getInt(o["bri"], v) || v < 0 || v > 255) return "bri must be 0-255";
      ns.bri = v;
    }
    for (uint8_t k = 0; k < MAX_OUTPUTS; k++)
      if ((err = parseSeg(o, ns.seg[k], false)).length()) return err;
    syncAll = !o["effect"].isNull() || !o["speed"].isNull();
  }
  JsonVariantConst list = o["segments"];
  if (!list.isNull()) {
    if (!list.is<JsonArrayConst>()) return "segments must be an array";
    for (JsonObjectConst j : list.as<JsonArrayConst>()) {
      uint8_t k;
      if ((err = segIndex(j["id"], k)).length()) return err;
      if ((err = parseSeg(j, ns.seg[k], true)).length()) return err;
      if (ns.seg[k].on && !state.seg[k].on) ns.on = true;
    }
  }
  state = ns;
  if (syncAll) ledsSync();
  stateChanged();
  return "";
}

// --------------------------------------------------------------------------- captive portal
static bool isOwnHost(const String &host) {
  return host == netApIp().toString() || host == netIp().toString() ||
         host.equalsIgnoreCase(deviceHostname() + ".local") || host.equalsIgnoreCase(deviceHostname());
}

// When the setup AP is up, any request for a foreign host gets bounced to our UI,
// which makes phones/laptops pop up the "sign in to network" page.
static bool captiveRedirect() {
  if (!netApActive() || isOwnHost(server.hostHeader())) return false;
  server.sendHeader("Location", String("http://") + netApIp().toString() + "/", true);
  server.send(302, "text/plain", "");
  return true;
}

// --------------------------------------------------------------------------- handlers
static void handleRoot() {
  if (captiveRedirect()) return;
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", WEB_UI);
}

// Fixture model icons, baked into the firmware (see icons.h). Public and cached: they hold no data.
static void handleIcon() {
  String u = server.uri();
  const uint8_t *p = nullptr;
  size_t n = 0;
  if (u == "/icon/string.webp") p = ICON_STRING, n = sizeof(ICON_STRING);
  else if (u == "/icon/snowflake91.webp") p = ICON_SNOWFLAKE, n = sizeof(ICON_SNOWFLAKE);
  else if (u == "/icon/arches4x24.webp") p = ICON_ARCHES, n = sizeof(ICON_ARCHES);
  if (!p) return server.send(404, "text/plain", "not found");
  server.sendHeader("Cache-Control", "max-age=604800");
  server.send_P(200, "image/webp", (PGM_P)p, n);
}

static void handleInfo() {
  JsonDocument d;
  // Identity block: always public, so an app can show the device and know to ask for a token.
  d["id"] = deviceId();
  d["name"] = cfg.name;
  d["model"] = FW_MODEL;
  d["manufacturer"] = FW_MANUFACTURER;
  d["firmware"] = FW_NAME;
  d["version"] = FW_VERSION;
  d["api"] = API_VERSION;
  d["auth"] = cfg.apiToken.length() > 0;
  d["wsPort"] = WS_PORT;
  d["fixture"] = fixtureModel();  // model of the first output, so apps can pick an icon
  d["board"] = BOARD_ID;       // hardware: "esp12" or "wt32-eth01" (picks the right firmware file)
  if (!authed()) return sendJson(200, d);

  d["hostname"] = deviceHostname();
  d["build"] = __DATE__ " " __TIME__;
  d["mac"] = WiFi.macAddress();
  char chip[7];
  snprintf(chip, sizeof(chip), "%06X", (unsigned)chipId24());
  d["chipId"] = chip;
  IPAddress ip = netIp();
  if (ip != IPAddress()) d["ip"] = ip.toString();  // home-network address, Ethernet or WiFi

  JsonObject cap = d["capabilities"].to<JsonObject>();
  cap["rgb"] = true;
  cap["white"] = false;
  cap["effects"] = EFFECT_COUNT;
  cap["speed"] = true;
  cap["ir"] = irEnabled();
  cap["identify"] = true;
  cap["websocket"] = true;
  cap["maxLeds"] = MAX_LEDS;
  cap["ota"] = true;
  cap["ethernet"] = netHasEth();
  cap["outputs"] = MAX_OUTPUTS;

  JsonObject w = d["wifi"].to<JsonObject>();
  w["mode"] = netModeStr();
  w["ssid"] = cfg.ssid;
  w["status"] = netStatusStr();
  w["connected"] = netStaConnected();
  if (netStaConnected()) {
    w["ip"] = WiFi.localIP().toString();
    w["rssi"] = WiFi.RSSI();
  }
  w["apActive"] = netApActive();
  if (netApActive()) {
    w["apSsid"] = netApSsid();
    w["apIp"] = netApIp().toString();
  }

  if (netHasEth()) {
    JsonObject e = d["eth"].to<JsonObject>();
    e["link"] = netEthLink();
    e["connected"] = netEthUp();
    if (netEthUp()) e["ip"] = netEthIp().toString();
    if (netEthLink()) {
      e["speed"] = netEthSpeed();
      e["fullDuplex"] = netEthFullDuplex();
    }
    e["mac"] = netEthMac();
  }

  JsonObject l = d["leds"].to<JsonObject>();
  const Output &o0 = cfg.out[firstOutput()];
  l["pin"] = o0.pin;  // first output (single-strip apps)
  l["count"] = totalLeds();
  l["colorOrder"] = o0.order;
  JsonArray outs = l["outputs"].to<JsonArray>();
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    if (!outputUsed(k)) continue;
    JsonObject j = outs.add<JsonObject>();
    j["id"] = k;
    j["name"] = outputName(k);
    j["pin"] = cfg.out[k].pin;
    j["count"] = cfg.out[k].count;
    j["colorOrder"] = cfg.out[k].order;
    j["model"] = cfg.out[k].model;
    j["active"] = cfg.out[k].active;  // false = hidden on the web UI's control card
  }
  l["estimatedMa"] = ledsEstimatedMa();
  l["maxMa"] = cfg.maxMa;
  l["powerLimited"] = ledsPowerLimited();

  JsonObject ir = d["ir"].to<JsonObject>();
  ir["pin"] = cfg.irPin;
  ir["enabled"] = irEnabled();
  if (irLastCode()) {
    char hex[11];
    snprintf(hex, sizeof(hex), "0x%06X", (unsigned)irLastCode());
    ir["lastCode"] = hex;
    ir["lastAgeMs"] = irLastAgeMs();
  }

  JsonObject sys = d["system"].to<JsonObject>();
  sys["uptimeS"] = millis() / 1000;
  sys["freeHeap"] = ESP.getFreeHeap();
  sys["sketchSize"] = ESP.getSketchSize();
  sys["freeSketchSpace"] = ESP.getFreeSketchSpace();
  sys["flashSize"] = flashSize();
  sys["resetReason"] = resetReason();
#if defined(ESP32)
  // Flash layout as found on the chip (OTA can't change it, so it may be the previous firmware's).
  JsonArray parts = sys["partitions"].to<JsonArray>();
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  for (; it; it = esp_partition_next(it)) {
    const esp_partition_t *pt = esp_partition_get(it);
    JsonObject j = parts.add<JsonObject>();
    j["label"] = pt->label;
    j["type"] = pt->type;
    j["subtype"] = pt->subtype;
    char hex[11];
    snprintf(hex, sizeof(hex), "0x%x", (unsigned)pt->address);
    j["offset"] = hex;
    snprintf(hex, sizeof(hex), "0x%x", (unsigned)pt->size);
    j["size"] = hex;
  }
  esp_partition_iterator_release(it);
#endif
  sendJson(200, d);
}

static void handleEffects() {
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
    JsonObject e = a.add<JsonObject>();
    e["id"] = i;
    e["name"] = EFFECT_NAMES[i];
    e["colors"] = effectColors(i);  // how many of colors 1-3 it uses (0 = its own palette)
  }
  sendJson(200, d);
}

static void handleStatePost() {
  JsonDocument d;
  if (!readBody(d)) return;
  String err = applyState(d.as<JsonObjectConst>());
  if (err.length()) return sendErr(400, err);
  sendState();
}

// GET /api/set?on=toggle&bri=200&color=ff8800&effect=rainbow&speed=100
static void handleSetGet() {
  JsonDocument d;
  for (int i = 0; i < server.args(); i++)
    if (server.argName(i) != "plain" && server.argName(i) != "token") d[server.argName(i)] = server.arg(i);
  String err = applyState(d.as<JsonObjectConst>());
  if (err.length()) return sendErr(400, err);
  sendState();
}

static void handleConfigGet() {
  JsonDocument d;
  d["name"] = cfg.name;
  d["hostname"] = deviceHostname();
  d["ssid"] = cfg.ssid;
  d["hasPass"] = cfg.pass.length() > 0;
  d["hasApPass"] = cfg.apPass.length() > 0;
  d["ledPin"] = cfg.out[0].pin;  // output 1, for single-strip apps
  d["ledCount"] = cfg.out[0].count;
  d["colorOrder"] = cfg.out[0].order;
  d["maxOutputs"] = MAX_OUTPUTS;
  d["maxLeds"] = MAX_LEDS;
  JsonArray outs = d["outputs"].to<JsonArray>();
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    JsonObject j = outs.add<JsonObject>();
    j["pin"] = cfg.out[k].pin;  // -1 = not used
    j["count"] = cfg.out[k].count;
    j["order"] = cfg.out[k].order;
    j["name"] = cfg.out[k].name;
    j["model"] = cfg.out[k].model;
    j["active"] = cfg.out[k].active;  // false = hidden on the web UI's control card
  }
  d["irPin"] = cfg.irPin;
  d["maxMa"] = cfg.maxMa;
  d["fixture"] = fixtureModel();
  d["setupSsid"] = setupApSsid();
  d["apiToken"] = cfg.apiToken;  // caller is already authenticated (or auth is off)
  sendJson(200, d);
}

static void handleConfigPost() {
  JsonDocument d;
  if (!readBody(d)) return;
  Config c = cfg;
  long v;
  if (!d["name"].isNull()) {
    String n = d["name"].as<String>();
    n.trim();
    if (n.length() < 1 || n.length() > 32) return sendErr(400, "name must be 1-32 characters");
    c.name = n;
  }
  if (!d["apPass"].isNull()) {
    String p = d["apPass"].as<String>();
    if (p.length() && (p.length() < 8 || p.length() > 63))
      return sendErr(400, "AP password must be empty (open) or 8-63 characters");
    c.apPass = p;
  }
  // outputs: [{pin, count, order, name}, ...] (pin -1 = not used); ledPin/ledCount/colorOrder
  // still set output 1 for single-strip apps.
  auto parseOut = [&](JsonObjectConst j, Output &o, bool allowUnused) -> String {
    long v;
    if (!j["pin"].isNull()) {
      if (!getInt(j["pin"], v) || !((allowUnused && v == -1) || validLedPin(v))) return "invalid LED GPIO";
      o.pin = v;
    }
    if (!j["count"].isNull()) {
      if (!getInt(j["count"], v) || v < 1 || v > MAX_LEDS) return String("LED count must be 1-") + MAX_LEDS;
      o.count = v;
    }
    if (!j["order"].isNull()) {
      String s = j["order"].as<String>();
      s.toUpperCase();
      if (!validColorOrder(s)) return "color order must be RGB, RBG, GRB, GBR, BRG or BGR";
      o.order = s;
    }
    if (!j["name"].isNull()) {
      String s = j["name"].as<String>();
      s.trim();
      if (s.length() > 24) return "output names can be at most 24 characters";
      o.name = s;
    }
    if (!j["active"].isNull()) {
      if (!j["active"].is<bool>()) return "active must be true or false";
      o.active = j["active"].as<bool>();
    }
    if (!j["model"].isNull()) {
      String m = j["model"].as<String>();
      if (!validModel(m)) return "model must be custom, string, snowflake91 or arches4x24";
      o.model = m;
    }
    return "";
  };
  // "fixture" = output 1's model (from before each output had its own); outputs[0].model wins
  if (!d["fixture"].isNull()) {
    String f = d["fixture"].as<String>();
    if (!validModel(f)) return sendErr(400, "fixture must be custom, string, snowflake91 or arches4x24");
    c.out[0].model = f;
  }
  JsonVariantConst outs = d["outputs"];
  if (!outs.isNull()) {
    if (!outs.is<JsonArrayConst>() || outs.size() > MAX_OUTPUTS)
      return sendErr(400, String("outputs must be an array of up to ") + MAX_OUTPUTS);
    for (uint8_t k = 0; k < outs.size(); k++) {
      String err = parseOut(outs[k], c.out[k], true);
      if (err.length()) return sendErr(400, "output " + String(k + 1) + ": " + err);
    }
  }
  {
    JsonDocument legacy;
    if (!d["ledPin"].isNull()) legacy["pin"] = d["ledPin"];
    if (!d["ledCount"].isNull()) legacy["count"] = d["ledCount"];
    if (!d["colorOrder"].isNull()) legacy["order"] = d["colorOrder"];
    String err = parseOut(legacy.as<JsonObjectConst>(), c.out[0], false);
    if (err.length()) return sendErr(400, err);
  }
  if (!d["irPin"].isNull()) {
    if (!getInt(d["irPin"], v) || !validIrPin(v)) return sendErr(400, "invalid IR GPIO");
    c.irPin = v;
  }
  if (!d["maxMa"].isNull()) {
    if (!getInt(d["maxMa"], v) || (v != 0 && (v < 100 || v > 20000)))
      return sendErr(400, "maxMa must be 0 (no limit) or 100-20000");
    c.maxMa = v;
  }
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++)  // some models have a fixed pixel count
    if (uint16_t n = modelFixedCount(c.out[k].model)) c.out[k].count = n;
  {
    uint16_t total = 0;
    bool any = false;
    for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
      const Output &o = c.out[k];
      if (o.pin < 0) continue;
      any = true;
      total += o.count;
      for (uint8_t j = 0; j < k; j++)
        if (c.out[j].pin == o.pin) return sendErr(400, "outputs " + String(j + 1) + " and " + String(k + 1) + " use the same GPIO");
      if (c.irPin >= 0 && c.irPin == o.pin) return sendErr(400, "LED and IR can't share a GPIO");
    }
    if (!any) return sendErr(400, "at least one output needs a GPIO");
    if (total > MAX_LEDS) return sendErr(400, String("too many LEDs: ") + total + " in total, the limit is " + MAX_LEDS);
  }
  if (!d["apiToken"].isNull()) {
    String t = d["apiToken"].as<String>();
    t.trim();
    if (t == "generate") t = generateToken();
    if (t.length()) {
      bool okChars = t.length() >= 16 && t.length() <= 64;
      for (unsigned i = 0; okChars && i < t.length(); i++)
        okChars = isalnum((unsigned char)t[i]) || t[i] == '-' || t[i] == '_';
      if (!okChars) return sendErr(400, "apiToken must be 16-64 chars of A-Z a-z 0-9 - _, \"generate\", or \"\"");
    }
    c.apiToken = t;
  }

  // Name and auth on/off are part of the mDNS announcement, so they need a restart too.
  bool outsChanged = false, orderChanged = false;
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    if (c.out[k].pin != cfg.out[k].pin || c.out[k].count != cfg.out[k].count) outsChanged = true;
    if (c.out[k].order != cfg.out[k].order) orderChanged = true;
  }
  bool reboot = c.name != cfg.name || outsChanged || c.irPin != cfg.irPin || (c.apiToken.length() > 0) != (cfg.apiToken.length() > 0);
  bool tokenChanged = c.apiToken != cfg.apiToken;
  cfg = c;
  if (!configSave()) return sendErr(500, "could not save settings");
  if (orderChanged && !reboot) ledsUpdateOrders();
  JsonDocument r;
  r["ok"] = true;
  if (reboot) r["reboot"] = true;
  if (tokenChanged) r["apiToken"] = cfg.apiToken;  // shown once to the caller that set it
  sendJson(200, r);
  if (reboot) scheduleReboot(1500);
}

static void handleWifiScan() {
  JsonDocument d;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    netPrepareScan();
    WiFi.scanNetworks(true, false);
    d["scanning"] = true;
  } else if (n == WIFI_SCAN_RUNNING) {
    d["scanning"] = true;
  } else {
    d["scanning"] = false;
    JsonArray a = d["networks"].to<JsonArray>();
    // de-duplicate by SSID keeping the strongest, sorted strongest first
    bool used[64] = {false};
    int m = min(n, 64);
    for (int k = 0; k < m; k++) {
      int best = -1;
      for (int i = 0; i < m; i++)
        if (!used[i] && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
      used[best] = true;
      String s = WiFi.SSID(best);
      if (!s.length()) continue;
      bool dup = false;
      for (JsonObject e : a)
        if (s == e["ssid"].as<const char *>()) dup = true;
      if (dup) continue;
      JsonObject e = a.add<JsonObject>();
      e["ssid"] = s;
      e["rssi"] = WiFi.RSSI(best);
      #if defined(ESP32)
      e["secure"] = WiFi.encryptionType(best) != WIFI_AUTH_OPEN;
#else
      e["secure"] = WiFi.encryptionType(best) != ENC_TYPE_NONE;
#endif
      e["channel"] = WiFi.channel(best);
    }
    WiFi.scanDelete();
  }
  sendJson(200, d);
}

static void handleWifiPost() {
  JsonDocument d;
  if (!readBody(d)) return;
  String ssid = d["ssid"] | "";
  String pass = d["pass"] | "";
  ssid.trim();
  if (ssid.length() > 32) return sendErr(400, "SSID too long");
  if (pass.length() && (pass.length() < 8 || pass.length() > 63))
    return sendErr(400, "password must be empty (open network) or 8-63 characters");
  cfg.ssid = ssid;
  cfg.pass = pass;
  if (!configSave()) return sendErr(500, "could not save settings");
  if (!ssid.length()) {  // forget network -> back to setup AP
    sendOk(true);
    scheduleReboot(1000);
    return;
  }
  // Try the new network right away. If the setup AP is up it stays up (AP+STA) so
  // the browser can watch /api/info for the result. With Ethernet up it's kept as the backup.
  JsonDocument r;
  r["ok"] = true;
  if (netEthUp()) r["standby"] = true;
  sendJson(200, r);
  netConnect();
}

static void handleUpdateDone() {
  bool ok = !Update.hasError() && updateError.length() == 0;
  server.sendHeader("Connection", "close");
  if (ok) {
    sendOk(true);
    scheduleReboot(1000);
  } else if (updateError.startsWith("unauthorized")) {
    sendErr(401, updateError);
  } else {
    sendErr(updateError.startsWith("wrong board") ? 400 : 500, updateError.length() ? updateError : String("update failed"));
    ledsPause(false);
  }
}

static void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    updateError = "";
    if (!authed()) {  // body arrives before the final handler runs, so check here
      updateError = "unauthorized: API token required";
      return;
    }
    ledsPause(true);
    Serial.printf("[ota] receiving %s\n", up.filename.c_str());
    updateFirstChunk = true;
    if (!updateBegin()) updateError = updateErrorStr();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (updateFirstChunk && !updateError.length()) {
      updateFirstChunk = false;
      if (!firmwareImageOk(up.buf, up.currentSize)) {
        updateError = "wrong board: this firmware file is not for " BOARD_ID;
        Update.end();  // partial image: discards it on both chips
      }
    }
    if (!updateError.length() && Update.write(up.buf, up.currentSize) != up.currentSize)
      updateError = updateErrorStr();
  } else if (up.status == UPLOAD_FILE_END) {
    if (!updateError.length()) {
      if (Update.end(true))
        Serial.printf("[ota] success, %u bytes\n", up.totalSize);
      else
        updateError = updateErrorStr();
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    updateError = "upload aborted";
  }
  delay(0);
}

static void handleIdentify() {
  ledsIdentify();
  sendOk();
}

// --------------------------------------------------------------------------- WebSocket (port 81)
// Server -> client: {"type":"state","state":{...}} on connect and on every change (any source:
// web UI, API, IR remote). Client -> server: a state object like POST /api/state
// (bare, or wrapped as {"type":"set","state":{...}}), {"type":"get"} or {"type":"identify"}.
static String stateMessage() {
  JsonDocument d;
  d["type"] = "state";
  stateToJson(d["state"].to<JsonObject>());
  String s;
  serializeJson(d, s);
  return s;
}

static void wsSendError(uint8_t num, const String &err) {
  JsonDocument d;
  d["type"] = "error";
  d["error"] = err;
  String s;
  serializeJson(d, s);
  ws.sendTXT(num, s);
}

static void wsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t len) {
  if (type == WStype_CONNECTED) {
    if (cfg.apiToken.length()) {
      String url = String((const char *)payload);  // e.g. "/?token=abc"
      String tok;
      int i = url.indexOf("token=");
      if (i >= 0) {
        tok = url.substring(i + 6);
        int amp = tok.indexOf('&');
        if (amp >= 0) tok = tok.substring(0, amp);
      }
      if (!tokenEquals(tok, cfg.apiToken)) {
        wsSendError(num, "unauthorized: API token required");
        ws.disconnect(num);
        return;
      }
    }
    String m = stateMessage();
    ws.sendTXT(num, m);
  } else if (type == WStype_TEXT) {
    JsonDocument d;
    if (deserializeJson(d, payload, len) || !d.is<JsonObject>()) return wsSendError(num, "message must be a JSON object");
    String t = d["type"] | "set";
    if (t == "identify") {
      ledsIdentify();
    } else if (t == "get") {
      String m = stateMessage();
      ws.sendTXT(num, m);
    } else if (t == "set") {
      JsonObjectConst o = d["state"].is<JsonObjectConst>() ? d["state"].as<JsonObjectConst>() : d.as<JsonObjectConst>();
      String err = applyState(o);
      if (err.length()) wsSendError(num, err);
      // success is confirmed by the state broadcast in webLoop()
    } else {
      wsSendError(num, "unknown type");
    }
  }
}

static void handleReboot() {
  sendOk(true);
  scheduleReboot(800);
}

static void handleReset() {
  sendOk(true);
  factoryReset();
}

static void handleCaptiveProbe() {
  if (captiveRedirect()) return;
  server.send(204);
}

static void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {  // CORS preflight
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Api-Key");
    server.send(204);
    return;
  }
  if (captiveRedirect()) return;
  sendErr(404, "not found");
}

// --------------------------------------------------------------------------- setup
void webBegin() {
  server.enableCORS(true);  // lets other pages / dashboards call the API
#if defined(ESP32)
  static const char *headers[] = {"Authorization", "X-Api-Key"};
  server.collectHeaders(headers, 2);
#else
  server.collectHeaders("Authorization", "X-Api-Key");
#endif

  server.on("/", HTTP_GET, handleRoot);

  server.on("/icon/string.webp", HTTP_GET, handleIcon);
  server.on("/icon/snowflake91.webp", HTTP_GET, handleIcon);
  server.on("/icon/arches4x24.webp", HTTP_GET, handleIcon);
  server.on("/api/info", HTTP_GET, handleInfo);  // public identity block; full details need auth
  server.on("/api/state", HTTP_GET, guarded<sendState>);
  server.on("/api/state", HTTP_POST, guarded<handleStatePost>);
  server.on("/api/set", HTTP_GET, guarded<handleSetGet>);
  server.on("/api/effects", HTTP_GET, guarded<handleEffects>);
  server.on("/api/identify", HTTP_POST, guarded<handleIdentify>);
  server.on("/api/config", HTTP_GET, guarded<handleConfigGet>);
  server.on("/api/config", HTTP_POST, guarded<handleConfigPost>);
  server.on("/api/wifi/scan", HTTP_GET, guarded<handleWifiScan>);
  server.on("/api/wifi", HTTP_POST, guarded<handleWifiPost>);
  server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);  // auth checked in upload
  server.on("/api/reboot", HTTP_POST, guarded<handleReboot>);
  server.on("/api/reset", HTTP_POST, guarded<handleReset>);

  // OS connectivity checks -> captive portal pop-up
  for (const char *p : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                        "/ncsi.txt", "/connecttest.txt", "/fwlink", "/redirect", "/canonical.html"})
    server.on(p, handleCaptiveProbe);

  server.onNotFound(handleNotFound);
  server.begin();
  ws.onEvent(wsEvent);
  ws.begin();
  wsSentVersion = stateVersion;
  Serial.printf("[web] HTTP on port 80, WebSocket on port %d%s\n", WS_PORT,
                cfg.apiToken.length() ? " (API token required)" : "");
}

void webLoop() {
  server.handleClient();
  ws.loop();
  // Push state to WebSocket clients when it changes, at most every 50 ms (slider drags).
  if (stateVersion != wsSentVersion && millis() - wsLastPush >= 50) {
    wsSentVersion = stateVersion;
    wsLastPush = millis();
    if (ws.connectedClients() > 0) {
      String m = stateMessage();
      ws.broadcastTXT(m);
    }
  }
}
