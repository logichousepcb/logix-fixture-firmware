#include "web.h"

#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <WebSocketsServer.h>

#include "config.h"
#include "icons.h"
#include "ir.h"
#include "leds.h"
#include "net.h"
#include "web_ui.h"

static ESP8266WebServer server(80);
static WebSocketsServer ws(WS_PORT);
static String updateError;
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

static void stateToJson(JsonObject o) {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02x%02x%02x", state.r, state.g, state.b);
  o["on"] = state.on;
  o["bri"] = state.bri;
  JsonArray c = o["color"].to<JsonArray>();
  c.add(state.r);
  c.add(state.g);
  c.add(state.b);
  o["hex"] = hex;
  o["effect"] = state.effect;
  o["effectName"] = EFFECT_NAMES[state.effect];
  o["speed"] = state.speed;
}

static void sendState() {
  JsonDocument d;
  stateToJson(d.to<JsonObject>());
  sendJson(200, d);
}

// Validates everything first, then applies. Returns "" on success or an error message.
static String applyState(JsonObjectConst o) {
  State s = state;
  long v;
  JsonVariantConst on = o["on"];
  if (!on.isNull()) {
    if (on.is<bool>()) {
      s.on = on.as<bool>();
    } else {
      String t = on.as<String>();
      t.toLowerCase();
      if (t == "toggle" || t == "t") s.on = !state.on;
      else if (t == "1" || t == "true" || t == "on") s.on = true;
      else if (t == "0" || t == "false" || t == "off") s.on = false;
      else return "on must be true, false or \"toggle\"";
    }
  }
  if (!o["bri"].isNull()) {
    if (!getInt(o["bri"], v) || v < 0 || v > 255) return "bri must be 0-255";
    s.bri = v;
  }
  JsonVariantConst col = o["color"];
  if (!col.isNull()) {
    if (col.is<JsonArrayConst>() && col.size() >= 3) {
      long c[3];
      for (int i = 0; i < 3; i++)
        if (!getInt(col[i], c[i]) || c[i] < 0 || c[i] > 255) return "color values must be 0-255";
      s.r = c[0];
      s.g = c[1];
      s.b = c[2];
    } else if (!parseHex(col.as<String>(), s.r, s.g, s.b)) {
      return "color must be [r,g,b] or \"#rrggbb\"";
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
  state = s;
  stateChanged();
  return "";
}

// --------------------------------------------------------------------------- captive portal
static bool isOwnHost(const String &host) {
  return host == netApIp().toString() || host == WiFi.localIP().toString() ||
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
  d["fixture"] = cfg.fixture;  // fixture model, so apps can pick an icon
  if (!authed()) return sendJson(200, d);

  d["hostname"] = deviceHostname();
  d["build"] = __DATE__ " " __TIME__;
  d["mac"] = WiFi.macAddress();
  char chip[7];
  snprintf(chip, sizeof(chip), "%06X", (unsigned)ESP.getChipId());
  d["chipId"] = chip;

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

  JsonObject l = d["leds"].to<JsonObject>();
  l["pin"] = cfg.ledPin;
  l["count"] = cfg.ledCount;
  l["colorOrder"] = cfg.colorOrder;
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
  sys["flashSize"] = ESP.getFlashChipRealSize();
  sys["resetReason"] = ESP.getResetReason();
  sendJson(200, d);
}

static void handleEffects() {
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
    JsonObject e = a.add<JsonObject>();
    e["id"] = i;
    e["name"] = EFFECT_NAMES[i];
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
  d["ledPin"] = cfg.ledPin;
  d["ledCount"] = cfg.ledCount;
  d["colorOrder"] = cfg.colorOrder;
  d["irPin"] = cfg.irPin;
  d["maxMa"] = cfg.maxMa;
  d["fixture"] = cfg.fixture;
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
  if (!d["ledPin"].isNull()) {
    if (!getInt(d["ledPin"], v) || !validLedPin(v)) return sendErr(400, "invalid LED GPIO");
    c.ledPin = v;
  }
  if (!d["ledCount"].isNull()) {
    if (!getInt(d["ledCount"], v) || v < 1 || v > MAX_LEDS)
      return sendErr(400, String("ledCount must be 1-") + MAX_LEDS);
    c.ledCount = v;
  }
  if (!d["colorOrder"].isNull()) {
    String o = d["colorOrder"].as<String>();
    o.toUpperCase();
    if (!validColorOrder(o)) return sendErr(400, "colorOrder must be RGB, RBG, GRB, GBR, BRG or BGR");
    c.colorOrder = o;
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
  if (!d["fixture"].isNull()) {
    String f = d["fixture"].as<String>();
    if (!validFixture(f)) return sendErr(400, "fixture must be custom, string or snowflake91");
    c.fixture = f;
  }
  if (c.fixture == "snowflake91") c.ledCount = 91;  // fixed pixel count for this model
  if (c.irPin >= 0 && c.irPin == c.ledPin) return sendErr(400, "LED and IR can't share a GPIO");
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
  bool reboot = c.name != cfg.name || c.ledPin != cfg.ledPin || c.ledCount != cfg.ledCount ||
                c.irPin != cfg.irPin || (c.apiToken.length() > 0) != (cfg.apiToken.length() > 0);
  bool orderChanged = c.colorOrder != cfg.colorOrder;
  bool tokenChanged = c.apiToken != cfg.apiToken;
  cfg = c;
  if (!configSave()) return sendErr(500, "could not save settings");
  if (orderChanged) ledsSetOrder(cfg.colorOrder);
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
      e["secure"] = WiFi.encryptionType(best) != ENC_TYPE_NONE;
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
  sendOk(false);
  // Try the new network right away. If the setup AP is up it stays up (AP+STA) so
  // the browser can watch /api/info for the result.
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
    sendErr(500, updateError.length() ? updateError : String("update failed"));
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
    uint32_t space = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(space, U_FLASH)) updateError = Update.getErrorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!updateError.length() && Update.write(up.buf, up.currentSize) != up.currentSize)
      updateError = Update.getErrorString();
  } else if (up.status == UPLOAD_FILE_END) {
    if (!updateError.length()) {
      if (Update.end(true))
        Serial.printf("[ota] success, %u bytes\n", up.totalSize);
      else
        updateError = Update.getErrorString();
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
  server.collectHeaders("Authorization", "X-Api-Key");

  server.on("/", HTTP_GET, handleRoot);

  server.on("/icon/string.webp", HTTP_GET, handleIcon);
  server.on("/icon/snowflake91.webp", HTTP_GET, handleIcon);
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
