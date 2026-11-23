#pragma once
#include <Arduino.h>

void irBegin();  // uses cfg.irPin (-1 = disabled)
void irLoop();
bool irEnabled();
uint32_t irLastCode();   // last NEC code received (0 = none yet)
uint32_t irLastAgeMs();  // ms since last code
