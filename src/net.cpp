#include "net.h"

#include <DNSServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

#include "config.h"

// WiFi behaviour:
//  * No saved network       -> setup AP "<name> Setup" + captive portal, forever.
//  * Saved network          -> station mode. If not connected within 30 s (or the
//                              link drops for 30 s), the setup AP comes up as a fallback
//                              while the station keeps retrying in the background.
//  * Once the station is connected for 60 s, the fallback AP shuts down.
// While a phone is connected to the setup AP we pause station retries, because the
// ESP8266 radio hops channels while searching and that makes the AP unusable.

static const IPAddress AP_IP(192, 168, 4, 1);
static const uint32_t FALLBACK_AFTER_MS = 30000;
static const uint32_t AP_LINGER_MS = 60000;
static const uint32_t RETRY_EVERY_MS = 60000;
static const uint32_t ATTEMPT_MS = 15000;
static const uint32_t TEST_MS = 25000;

static DNSServer dns;
static bool apOn = false;
static bool mdnsOn = false;
static bool staIdle = false;      // we deliberately stopped station retries
static uint32_t connectedSince = 0;
static uint32_t lostSince = 0;
static uint32_t lastAttempt = 0;
static uint32_t testUntil = 0;    // user just submitted credentials: don't interrupt
static String apSsid;
static wl_status_t lastFail = WL_DISCONNECTED;

bool netApActive() { return apOn; }
bool netStaConnected() { return WiFi.status() == WL_CONNECTED; }
String netApSsid() { return apSsid; }
IPAddress netApIp() { return AP_IP; }

const char *netModeStr() {
  bool sta = cfg.ssid.length() > 0;
  if (apOn && sta) return "ap+sta";
  return apOn ? "ap" : "sta";
}

static const char *statusWords(wl_status_t s) {
  switch (s) {
    case WL_CONNECTED: return "connected";
    case WL_NO_SSID_AVAIL: return "network not found";
    case WL_CONNECT_FAILED: return "connection failed";
    case WL_WRONG_PASSWORD: return "wrong password";
    case WL_IDLE_STATUS: return "idle";
    default: return "connecting";
  }
}

const char *netStatusStr() {
  if (!cfg.ssid.length()) return "not configured";
  wl_status_t s = WiFi.status();
  // While retries are paused, report why the last attempt failed.
  if (staIdle && s != WL_CONNECTED) return statusWords(lastFail);
  return statusWords(s);
}

static void startAP() {
  if (apOn) return;
  WiFi.mode(cfg.ssid.length() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  apSsid = setupApSsid();  // fixed pattern so the phone app can find/join it
  if (cfg.apPass.length() >= 8)
    WiFi.softAP(apSsid, cfg.apPass);
  else
    WiFi.softAP(apSsid);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", AP_IP);
  apOn = true;
  Serial.printf("[net] setup AP '%s' up at %s\n", apSsid.c_str(), AP_IP.toString().c_str());
}

static void stopAP() {
  if (!apOn) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apOn = false;
  Serial.println("[net] setup AP off");
}

static void beginSta() {
  WiFi.begin(cfg.ssid, cfg.pass);
  lastAttempt = millis();
  staIdle = false;
}

void netBegin() {
  WiFi.persistent(false);  // credentials live in our config, not the SDK's flash area
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.hostname(deviceHostname());
  lostSince = millis();
  if (!cfg.ssid.length()) {
    startAP();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  beginSta();
  Serial.printf("[net] connecting to '%s'\n", cfg.ssid.c_str());
}

void netConnect() {
  if (!cfg.ssid.length()) return;
  if (apOn) WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();
  beginSta();
  testUntil = millis() + TEST_MS;
  Serial.printf("[net] trying '%s'\n", cfg.ssid.c_str());
}

static void startMdns() {
  if (mdnsOn) return;
  if (!MDNS.begin(deviceHostname())) return;
  // App discovery: _logix._tcp, following the TXT scheme used by the other LoGiX devices
  // (manufacturer, api, fw = version, name, id = MAC, model). The instance name stays the
  // hostname like those devices; the friendly name is in TXT "name".
  MDNS.addService(MDNS_SERVICE, "tcp", 80);
  MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "manufacturer", FW_MANUFACTURER);
  MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "api", String(API_VERSION));
  MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "fw", FW_VERSION);
  MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "name", cfg.name);
  MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "id", deviceId());
  MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "model", FW_MODEL);
  // extras specific to this firmware
  MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "ws", String(WS_PORT));
  MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "auth", cfg.apiToken.length() ? "1" : "0");
  // Plain web service so browsers / generic Bonjour tools still find the web UI.
  MDNS.addService("http", "tcp", 80);
  MDNS.addServiceTxt("http", "tcp", "id", deviceId());
  MDNS.addServiceTxt("http", "tcp", "model", FW_MODEL);
  mdnsOn = true;
  Serial.printf("[net] mDNS: %s.local, _%s._tcp id=%s\n", deviceHostname().c_str(), MDNS_SERVICE,
                deviceId().c_str());
}

void netLoop() {
  uint32_t now = millis();
  if (apOn) dns.processNextRequest();
  if (mdnsOn) MDNS.update();
  if (!cfg.ssid.length()) return;  // pure setup mode

  if (WiFi.status() == WL_CONNECTED) {
    if (!connectedSince) {
      connectedSince = now;
      lostSince = 0;
      WiFi.setAutoReconnect(true);
      Serial.printf("[net] connected to '%s', IP %s, http://%s.local\n", cfg.ssid.c_str(),
                    WiFi.localIP().toString().c_str(), deviceHostname().c_str());
      startMdns();
    }
    if (apOn && now - connectedSince > AP_LINGER_MS) stopAP();
    return;
  }

  // not connected
  if (connectedSince) {
    connectedSince = 0;
    lostSince = now;
    Serial.println("[net] WiFi lost");
  }
  if (!apOn && now - lostSince > FALLBACK_AFTER_MS) {
    WiFi.setAutoReconnect(false);  // we manage retries while the AP is up
    startAP();
  }
  if (!apOn) return;

  bool testing = (int32_t)(testUntil - now) > 0;
  bool attempting = testing || (!staIdle && now - lastAttempt < ATTEMPT_MS);
  if (attempting) return;
  if (WiFi.softAPgetStationNum() > 0) {
    if (!staIdle) {
      lastFail = WiFi.status();
      WiFi.disconnect();
      staIdle = true;
    }
  } else if (now - lastAttempt > RETRY_EVERY_MS) {
    beginSta();
  }
}
