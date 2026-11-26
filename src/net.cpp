#include "net.h"

#include <DNSServer.h>

#include "config.h"
#include "platform.h"

// Network behaviour:
//  * Ethernet (WT32-ETH01): always on. While it has an address, WiFi is switched off and any
//    saved WiFi network waits as a backup. When the cable is pulled, WiFi takes over a few
//    seconds later; when Ethernet comes back, WiFi is dropped again.
//  * No saved WiFi network  -> ESP8266: setup AP "LoGiX-Setup-XXXX" + captive portal, forever.
//                              Ethernet board: setup AP only while Ethernet is down (after a
//                              short grace period so the cable can get a DHCP address first).
//  * Saved network          -> station mode. If not online within 30 s (or the link drops for
//                              30 s), the setup AP comes up as a fallback while the station keeps
//                              retrying in the background.
//  * Once online for 60 s (by either interface), the fallback AP shuts down.
// While a phone is connected to the setup AP we pause station retries, because the radio hops
// channels while searching and that makes the AP unusable.

static const IPAddress AP_IP(192, 168, 4, 1);
static const uint32_t FALLBACK_AFTER_MS = 30000;
static const uint32_t AP_LINGER_MS = 60000;
static const uint32_t RETRY_EVERY_MS = 60000;
static const uint32_t ATTEMPT_MS = 15000;
static const uint32_t TEST_MS = 25000;
#if HAS_ETH
static const uint32_t ETH_GRACE_MS = 10000;  // no WiFi saved: wait this long for Ethernet before the AP
static const uint32_t STA_DELAY_MS = 6000;   // WiFi saved: give Ethernet a head start first
#else
static const uint32_t ETH_GRACE_MS = 0;
static const uint32_t STA_DELAY_MS = 0;
#endif

static DNSServer dns;
static bool apOn = false;
static bool mdnsOn = false;
static bool staOn = false;        // station started (WiFi.begin) and not deliberately stopped
static bool staIdle = false;      // we paused station retries for a phone on the setup AP
static uint32_t connectedSince = 0;
static uint32_t lostSince = 0;
static uint32_t lastAttempt = 0;
static uint32_t testUntil = 0;    // user just submitted credentials: don't interrupt
static String apSsid;
static wl_status_t lastFail = WL_DISCONNECTED;

// --------------------------------------------------------------------------- Ethernet
#if HAS_ETH
static volatile bool ethLink = false;
static volatile bool ethUp = false;
static bool ethSeen = false;  // ethUp as last handled by netLoop()

// Runs in the network event task: only flips flags, netLoop() does the work.
static void onNetEvent(WiFiEvent_t e) {
  switch (e) {
    case ARDUINO_EVENT_ETH_START: ETH.setHostname(deviceHostname().c_str()); break;
    case ARDUINO_EVENT_ETH_CONNECTED: ethLink = true; break;
    case ARDUINO_EVENT_ETH_GOT_IP: ethUp = true; break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP: ethLink = false; ethUp = false; break;
    default: break;
  }
}

bool netHasEth() { return true; }
bool netEthLink() { return ethLink; }
bool netEthUp() { return ethUp; }
IPAddress netEthIp() { return ethUp ? ETH.localIP() : IPAddress(); }
int netEthSpeed() { return ethLink ? ETH.linkSpeed() : 0; }
bool netEthFullDuplex() { return ethLink && ETH.fullDuplex(); }
String netEthMac() { return ETH.macAddress(); }
#else
bool netHasEth() { return false; }
bool netEthLink() { return false; }
bool netEthUp() { return false; }
IPAddress netEthIp() { return IPAddress(); }
int netEthSpeed() { return 0; }
bool netEthFullDuplex() { return false; }
String netEthMac() { return ""; }
#endif

// --------------------------------------------------------------------------- status
bool netApActive() { return apOn; }
bool netStaConnected() { return WiFi.status() == WL_CONNECTED; }
bool netConnected() { return netEthUp() || netStaConnected(); }
IPAddress netIp() {
  if (netEthUp()) return netEthIp();
  if (netStaConnected()) return WiFi.localIP();
  return IPAddress();
}
String netApSsid() { return apSsid; }
IPAddress netApIp() { return AP_IP; }

// WiFi is wanted when a network is saved and Ethernet isn't carrying us.
static bool staWanted() { return cfg.ssid.length() > 0 && !netEthUp(); }

const char *netModeStr() {
  bool sta = staWanted();
  if (apOn && sta) return "ap+sta";
  if (apOn) return "ap";
  return sta ? "sta" : "off";
}

static const char *statusWords(wl_status_t s) {
  switch (s) {
    case WL_CONNECTED: return "connected";
    case WL_NO_SSID_AVAIL: return "network not found";
    case WL_CONNECT_FAILED: return "connection failed";
#if !defined(ESP32)
    case WL_WRONG_PASSWORD: return "wrong password";
#endif
    case WL_IDLE_STATUS: return "idle";
    default: return "connecting";
  }
}

const char *netStatusStr() {
  if (!cfg.ssid.length()) return "not configured";
  if (netEthUp() && !netStaConnected()) return "standby (Ethernet connected)";
  wl_status_t s = WiFi.status();
  // While retries are paused, report why the last attempt failed.
  if (staIdle && s != WL_CONNECTED) return statusWords(lastFail);
  return statusWords(s);
}

// --------------------------------------------------------------------------- radio control
static WiFiMode_t wantedMode() {
  bool sta = staOn || staWanted();
  if (apOn) return sta ? WIFI_AP_STA : WIFI_AP;
  return sta ? WIFI_STA : WIFI_OFF;
}

static void startAP() {
  if (apOn) return;
  apOn = true;
  WiFi.mode(wantedMode());
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  apSsid = setupApSsid();  // fixed pattern so the phone app can find/join it
  if (cfg.apPass.length() >= 8)
    WiFi.softAP(apSsid.c_str(), cfg.apPass.c_str());
  else
    WiFi.softAP(apSsid.c_str());
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", AP_IP);
  Serial.printf("[net] setup AP '%s' up at %s\n", apSsid.c_str(), AP_IP.toString().c_str());
}

static void stopAP() {
  if (!apOn) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  apOn = false;
  WiFi.mode(wantedMode());
  Serial.println("[net] setup AP off");
}

static void beginSta() {
  staOn = true;
  staIdle = false;
  WiFi.mode(wantedMode());
#if defined(ESP32)
  WiFi.setSleep(false);  // modem sleep adds latency to every request
#endif
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  lastAttempt = millis();
}

#if HAS_ETH
static void stopSta() {
  if (!staOn) return;
  staOn = false;
  staIdle = false;
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  WiFi.mode(wantedMode());
}
#endif

void netPrepareScan() {
  if (!(WiFi.getMode() & WIFI_STA)) WiFi.mode(apOn ? WIFI_AP_STA : WIFI_STA);
}

// --------------------------------------------------------------------------- setup
void netBegin() {
  WiFi.persistent(false);  // credentials live in our config, not the SDK's flash area
#if defined(ESP32)
  WiFi.setHostname(deviceHostname().c_str());
#else
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.hostname(deviceHostname());
#endif
  lostSince = millis();
#if HAS_ETH
  WiFi.onEvent(onNetEvent);
  if (!ETH.begin(ETH01_PHY_ADDR, ETH01_PHY_POWER, ETH01_MDC, ETH01_MDIO, ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN))
    Serial.println("[net] Ethernet failed to start");
  else
    Serial.println("[net] Ethernet started, waiting for link");
#endif
  if (!cfg.ssid.length()) {
    if (!HAS_ETH) startAP();  // Ethernet boards wait ETH_GRACE_MS first (netLoop)
    else WiFi.mode(WIFI_OFF);
    return;
  }
  if (!STA_DELAY_MS) {
    WiFi.setAutoReconnect(true);
    beginSta();
    Serial.printf("[net] connecting to '%s'\n", cfg.ssid.c_str());
  } else {
    WiFi.mode(WIFI_OFF);
  }
}

void netConnect() {
  if (!cfg.ssid.length()) return;
  if (netEthUp()) {  // saved as the backup; it's used when Ethernet drops
    Serial.printf("[net] WiFi '%s' saved as backup (Ethernet connected)\n", cfg.ssid.c_str());
    return;
  }
  WiFi.disconnect();
  beginSta();
  testUntil = millis() + TEST_MS;
  Serial.printf("[net] trying '%s'\n", cfg.ssid.c_str());
}

static void startMdns() {
  if (mdnsOn) return;
  if (!MDNS.begin(deviceHostname().c_str())) return;
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
  MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "board", BOARD_ID);
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

// --------------------------------------------------------------------------- loop
void netLoop() {
  uint32_t now = millis();
  if (apOn) dns.processNextRequest();
#if !defined(ESP32)
  if (mdnsOn) MDNS.update();
#endif

#if HAS_ETH
  if (ethUp != ethSeen) {
    ethSeen = ethUp;
    if (ethUp) {
      Serial.printf("[net] Ethernet up: %s, %d Mbit/s %s duplex\n", ETH.localIP().toString().c_str(),
                    ETH.linkSpeed(), ETH.fullDuplex() ? "full" : "half");
      stopSta();  // Ethernet carries everything; keep WiFi as the backup
    } else {
      Serial.println("[net] Ethernet down");
    }
  }
#endif

  // Bring up the backup WiFi when Ethernet isn't available (after its head start).
  if (staWanted() && !staOn && !apOn && now - lostSince >= STA_DELAY_MS) {
    WiFi.setAutoReconnect(true);
    beginSta();
    Serial.printf("[net] connecting to '%s'\n", cfg.ssid.c_str());
  }

  if (netConnected()) {
    if (!connectedSince) {
      connectedSince = now;
      lostSince = 0;
      if (netEthUp())
        Serial.printf("[net] online via Ethernet, IP %s, http://%s.local\n", netIp().toString().c_str(),
                      deviceHostname().c_str());
      else
        Serial.printf("[net] connected to '%s', IP %s, http://%s.local\n", cfg.ssid.c_str(),
                      WiFi.localIP().toString().c_str(), deviceHostname().c_str());
      if (netStaConnected()) WiFi.setAutoReconnect(true);
      startMdns();
    }
    if (apOn && now - connectedSince > AP_LINGER_MS) stopAP();
    return;
  }

  // offline
  if (connectedSince) {
    connectedSince = 0;
    lostSince = now;
    Serial.println("[net] network lost");
  }
  if (!cfg.ssid.length() && !HAS_ETH) return;  // pure setup mode (AP already up)
  uint32_t wait = cfg.ssid.length() ? FALLBACK_AFTER_MS : ETH_GRACE_MS;
  if (!apOn && now - lostSince > wait) {
    WiFi.setAutoReconnect(false);  // we manage retries while the AP is up
    startAP();
  }
  if (!apOn || !staWanted()) return;

  bool testing = (int32_t)(testUntil - now) > 0;
  bool attempting = testing || (staOn && !staIdle && now - lastAttempt < ATTEMPT_MS);
  if (attempting) return;
  if (WiFi.softAPgetStationNum() > 0) {
    if (staOn && !staIdle) {
      lastFail = WiFi.status();
      WiFi.disconnect();
      staIdle = true;
    }
  } else if (!staOn || now - lastAttempt > RETRY_EVERY_MS) {
    beginSta();
  }
}
