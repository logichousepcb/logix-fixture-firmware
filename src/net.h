#pragma once
#include <Arduino.h>
#include <IPAddress.h>

void netBegin();
void netLoop();
void netConnect();         // (re)connect using cfg.ssid / cfg.pass without rebooting
bool netApActive();
bool netStaConnected();
bool netConnected();       // on the home network by Ethernet or WiFi
IPAddress netIp();         // address on the home network (Ethernet preferred); 0.0.0.0 if offline
String netApSsid();
IPAddress netApIp();
const char *netModeStr();   // "ap", "sta", "ap+sta", or "off" (WiFi unused because Ethernet is up)
const char *netStatusStr(); // station status in words
void netPrepareScan();      // make sure the radio can scan (it is off while Ethernet carries traffic)

// Ethernet (boards with HAS_ETH; all false/empty elsewhere)
bool netHasEth();
bool netEthLink();          // cable plugged in and link negotiated
bool netEthUp();            // link + DHCP address
IPAddress netEthIp();
int netEthSpeed();          // Mbit/s, 0 = no link
bool netEthFullDuplex();
String netEthMac();
