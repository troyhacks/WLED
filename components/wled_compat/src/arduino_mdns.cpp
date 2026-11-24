// arduino_mdns.cpp - the Arduino mDNS class, over ESP-IDF's mdns component.

#include "Arduino.h"
#include "ESPmDNS.h"

extern "C" {
#include "mdns.h"
#include "esp_log.h"
}

MDNSClass MDNS;

bool MDNSClass::begin(const char *hostname)
{
    if (mdns_init() != ESP_OK) {
        return false;
    }
    if (hostname) {
        mdns_hostname_set(hostname);
    }
    return true;
}

void MDNSClass::end()
{
    mdns_free();
}

bool MDNSClass::setHostname(const char *hostname)
{
    return mdns_hostname_set(hostname) == ESP_OK;
}

bool MDNSClass::setInstanceName(const char *instanceName)
{
    return mdns_instance_name_set(instanceName) == ESP_OK;
}

bool MDNSClass::addService(const char *instanceName, const char *type, uint16_t port)
{
    return addService(instanceName, type, port, nullptr);
}

bool MDNSClass::addService(const char *instanceName, const char *type, uint16_t port,
                           const char *domain)
{
    return addService(instanceName, type, port, domain, nullptr, nullptr);
}

bool MDNSClass::addService(const char *instanceName, const char *type, uint16_t port,
                           const char *domain, const char *service, const char *proto)
{
    return mdns_service_add(instanceName, type, port, domain, service, proto) == ESP_OK;
}

bool MDNSClass::removeService(const char *service, const char *proto)
{
    return mdns_service_remove(service, proto) == ESP_OK;
}

bool MDNSClass::updateService(const char *instanceName, const char *type, uint16_t port,
                              const char *domain)
{
    return mdns_service_update(instanceName, type, port, domain, nullptr, nullptr) == ESP_OK;
}

void MDNSClass::setTXTRecord(const char *service, const char *proto, const char *key,
                             const char *value)
{
    mdns_service_txt_set(service, proto, key, value, (uint8_t)strlen(value));
}

void MDNSClass::setTXTRecord(const char *service, const char *proto, const char *key,
                             uint8_t value)
{
    mdns_service_txt_set(service, proto, key, &value, 1);
}

void MDNSClass::setTXTRecord(const char *service, const char *proto, const char *key,
                             uint16_t value)
{
    mdns_service_txt_set(service, proto, key, &value, 2);
}

void MDNSClass::setTXTRecord(const char *service, const char *proto, const char *key,
                             uint32_t value)
{
    mdns_service_txt_set(service, proto, key, &value, 4);
}
