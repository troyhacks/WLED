// ETH.h - the Arduino Ethernet class.
//
// The P4's Ethernet is an ESP32 EMAC with an IP101 PHY, and WLED sets it up
// itself in wled00/wled.cpp with esp_eth/esp_netif.  This class is the
// Arduino-shaped view of that: it looks the netif up rather than owning it.

#pragma once

#include "Arduino.h"
#include "IPAddress.h"

class ETHClass {
public:
    bool config(const char *hostname = nullptr);
    bool setHostname(const char *hostname);
    IPAddress localIP();
    IPAddress subnetMask();
    IPAddress gatewayIP();
    uint8_t macAddress(uint8_t *mac);
    bool linkUp();
};

extern ETHClass ETH;
