// LoGiX Base Fixture firmware
// ESP-12F (ESP8266) + CH340B USB, WS2812 LED headers on GPIO2, TSOP38238 IR receiver on GPIO4.
// See README.md for hardware notes, setup, and the HTTP API.

#include <Arduino.h>

#include "config.h"
#include "ir.h"
#include "leds.h"
#include "net.h"
#include "web.h"

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("\n\n[sys] %s %s booting (%s)\n", FW_NAME, FW_VERSION, ESP.getResetReason().c_str());

  configBegin();

  // GPIO1/GPIO3 are the serial TX/RX pins; if the user assigned them to LEDs or IR, release serial.
  if (cfg.ledPin == 1 || cfg.ledPin == 3 || cfg.irPin == 1 || cfg.irPin == 3) {
    Serial.println("[sys] GPIO1/3 in use by LEDs/IR - serial logging disabled");
    Serial.flush();
    Serial.end();
  }

  ledsBegin();
  irBegin();
  netBegin();
  webBegin();
}

// USB serial commands (115200, newline-terminated). "factory-reset" is the recovery path
// if the API token or WiFi settings are lost: physical access to the board is enough.
static void serialLoop() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (line.length() < 40) line += c;
      continue;
    }
    line.trim();
    if (line == "factory-reset") {
      Serial.println("[sys] factory reset requested over serial");
      factoryReset();
    } else if (line == "info") {
      Serial.printf("[sys] %s %s id=%s name='%s' host=%s.local auth=%s\n", FW_NAME, FW_VERSION,
                    deviceId().c_str(), cfg.name.c_str(), deviceHostname().c_str(),
                    cfg.apiToken.length() ? "on" : "off");
    } else if (line.length()) {
      Serial.println("[sys] commands: info, factory-reset");
    }
    line = "";
  }
}

void loop() {
  serialLoop();
  netLoop();
  webLoop();
  irLoop();
  ledsLoop();
  stateLoop();
  rebootLoop();
}
