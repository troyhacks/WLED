// ESPmDNS.h - the Arduino mDNS class, over ESP-IDF's mdns component.
//
// The Arduino core's MDNS class wraps the esp_mdns component that IDF has
// had for a while; this is the same thing under the same name.

#pragma once

#include "Arduino.h"
#include "IPAddress.h"

class MDNSClass {
public:
    bool begin(const char *hostname);
    void end();
    bool setHostname(const char *hostname);
    bool setInstanceName(const char *instanceName);
    bool addService(const char *instanceName, const char *type, uint16_t port);
    bool addService(const char *instanceName, const char *type, uint16_t port,
                    const char *domain = nullptr);
    bool addService(const char *instanceName, const char *type, uint16_t port,
                    const char *domain, const char *service, const char *proto = nullptr);
    bool removeService(const char *service, const char *proto);
    bool updateService(const char *instanceName, const char *type, uint16_t port,
                       const char *domain = nullptr);
    void setTXTRecord(const char *service, const char *proto, const char *key, const char *value);
    void setTXTRecord(const char *service, const char *proto, const char *key, uint8_t value);
    void setTXTRecord(const char *service, const char *proto, const char *key, uint16_t value);
    void setTXTRecord(const char *service, const char *proto, const char *key, uint32_t value);
};

extern MDNSClass MDNS;
