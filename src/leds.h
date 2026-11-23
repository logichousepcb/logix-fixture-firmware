#pragma once
#include <Arduino.h>

extern const char *const EFFECT_NAMES[];
extern const uint8_t EFFECT_COUNT;

void ledsBegin();                      // uses cfg.ledPin / ledCount / colorOrder
void ledsLoop();
void ledsSetOrder(const String &order); // live color-order change
void ledsPause(bool paused);           // stop driving LEDs (used during firmware upload)
uint16_t ledsEstimatedMa();            // last estimated strip current
bool ledsPowerLimited();               // true if the current limiter is dimming output
void ledsIdentify();                   // blink white for ~3 s so the user can spot this fixture

// Call after changing `state` from anywhere: redraws and schedules a save.
void stateChanged();
