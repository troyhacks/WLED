// WiFi.h - the Arduino WiFi class, as far as the P4 needs it.
//
// The P4 has no radio of its own.  The C6 coprocessor provides Wi-Fi over
// esp-hosted, and this build runs Ethernet only, so all this class has to
// answer is "which address does this board have" - and the answer is the
// Ethernet address.
//
// The Arduino libraries include this header unconditionally, so it has to
// exist even though there is no Wi-Fi behind it.

#pragma once

#include "Arduino.h"
#include "IPAddress.h"

class WiFiClass {
public:
    IPAddress localIP();
    IPAddress softAPIP();
    IPAddress subnetMask();
    IPAddress gatewayIP();
    uint8_t macAddress(uint8_t *mac);
};

extern WiFiClass WiFi;
