#pragma once
#include <Arduino.h>

extern const char *const EFFECT_NAMES[];
extern const uint8_t EFFECT_COUNT;
bool effectUsesColor(uint8_t e);      // false for effects with their own palette (Rainbow, Christmas, ...)

void ledsBegin();                      // one driver per used output in cfg.out[]
void ledsLoop();
void ledsUpdateOrders();               // live color-order change (cfg.out[].order)
void ledsSync();                       // line up all outputs' animation clocks (same effect = in step)
void ledsPause(bool paused);           // stop driving LEDs (used during firmware upload)
uint16_t ledsEstimatedMa();            // last estimated current, all outputs
bool ledsPowerLimited();               // true if the current limiter is dimming output
void ledsIdentify();                   // blink white for ~3 s so the user can spot this fixture

// Call after changing `state` from anywhere: redraws and schedules a save.
void stateChanged();
