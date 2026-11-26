#include "ir.h"

#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>

#include "config.h"
#include "leds.h"

// Codes for the common 24-key "RGB LED" NEC remote (rows top to bottom).
enum : uint32_t {
  K_BRI_UP = 0xF700FF, K_BRI_DN = 0xF7807F, K_OFF = 0xF740BF, K_ON = 0xF7C03F,
  K_R1 = 0xF720DF, K_G1 = 0xF7A05F, K_B1 = 0xF7609F, K_W = 0xF7E01F,
  K_R2 = 0xF710EF, K_G2 = 0xF7906F, K_B2 = 0xF750AF, K_FLASH = 0xF7D02F,
  K_R3 = 0xF730CF, K_G3 = 0xF7B04F, K_B3 = 0xF7708F, K_STROBE = 0xF7F00F,
  K_R4 = 0xF708F7, K_G4 = 0xF78877, K_B4 = 0xF748B7, K_FADE = 0xF7C837,
  K_R5 = 0xF728D7, K_G5 = 0xF7A857, K_B5 = 0xF76897, K_SMOOTH = 0xF7E817,
};

static IRrecv *rx = nullptr;
static decode_results res;
static uint32_t lastCode = 0;
static uint32_t lastAt = 0;

bool irEnabled() { return rx != nullptr; }
uint32_t irLastCode() { return lastCode; }
uint32_t irLastAgeMs() { return lastCode ? millis() - lastAt : 0; }

void irBegin() {
  if (cfg.irPin < 0) return;
  rx = new IRrecv(cfg.irPin);
  rx->enableIRIn();
  Serial.printf("[ir] receiver on GPIO%d\n", cfg.irPin);
}

// The remote works like the "All" chip in the web UI: colors and effects go to every output.
static void color(uint8_t r, uint8_t g, uint8_t b) {
  for (Seg &s : state.seg) {
    s.r = r;
    s.g = g;
    s.b = b;
    s.effect = 0;
    s.on = true;
  }
  state.on = true;
}
static void effect(uint8_t e) {
  for (Seg &s : state.seg) {
    s.effect = e;
    s.on = true;
  }
  state.on = true;
  ledsSync();
}

static bool handle(uint32_t code) {
  switch (code) {
    case K_BRI_UP: state.bri = min(255, state.bri + 16); state.on = true; break;
    case K_BRI_DN: state.bri = max(5, state.bri - 16); break;
    case K_OFF: state.on = false; break;
    case K_ON: state.on = true; break;
    case K_R1: color(255, 0, 0); break;
    case K_G1: color(0, 255, 0); break;
    case K_B1: color(0, 0, 255); break;
    case K_W: color(255, 255, 255); break;
    case K_R2: color(255, 64, 0); break;
    case K_G2: color(0, 255, 96); break;
    case K_B2: color(48, 64, 255); break;
    case K_R3: color(255, 110, 0); break;
    case K_G3: color(0, 220, 255); break;
    case K_B3: color(110, 0, 255); break;
    case K_R4: color(255, 160, 0); break;
    case K_G4: color(0, 150, 255); break;
    case K_B4: color(200, 0, 255); break;
    case K_R5: color(255, 220, 0); break;
    case K_G5: color(0, 90, 255); break;
    case K_B5: color(255, 0, 160); break;
    case K_FLASH: effect(5); break;   // Theater Chase
    case K_STROBE: effect(6); break;  // Twinkle
    case K_FADE: effect(1); break;    // Breathe
    case K_SMOOTH: effect(3); break;  // Rainbow Cycle
    default: return false;
  }
  return true;
}

void irLoop() {
  if (!rx || !rx->decode(&res)) return;
  if (res.decode_type == NEC) {
    uint32_t now = millis();
    uint32_t code = 0;
    if (res.repeat) {
      // holding a key: only brightness keys auto-repeat
      if ((lastCode == K_BRI_UP || lastCode == K_BRI_DN) && now - lastAt < 400) code = lastCode;
      lastAt = now;
    } else {
      code = (uint32_t)res.value;
      lastCode = code;
      lastAt = now;
      Serial.printf("[ir] NEC 0x%06X\n", (unsigned)code);
    }
    if (code && handle(code)) stateChanged();
  }
  rx->resume();
}
