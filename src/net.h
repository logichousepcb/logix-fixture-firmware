#pragma once
#include <Arduino.h>
#include <IPAddress.h>

void netBegin();
void netLoop();
void netConnect();         // (re)connect using cfg.ssid / cfg.pass without rebooting
bool netApActive();
bool netStaConnected();
String netApSsid();
IPAddress netApIp();
const char *netModeStr();   // "ap", "sta", "ap+sta"
const char *netStatusStr(); // station status in words
