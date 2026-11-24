// arduino_eth.cpp - the Arduino Ethernet class, and the WiFi stub.
//
// WLED brings the P4's Ethernet up itself (wled00/wled.cpp): an ESP32 EMAC
// with an IP101 PHY, on the "ETH_DEF" netif.  These classes are the
// Arduino-shaped view of that netif - they look it up instead of owning it.

#include "Arduino.h"
#include "ETH.h"
#include "WiFi.h"

extern "C" {
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
}

ETHClass ETH;

namespace {

// The netif WLED's Ethernet lives on.
const char *ETH_NETIF_KEY = "ETH_DEF";

esp_netif_t *ethNetif()
{
    return esp_netif_get_handle_from_ifkey(ETH_NETIF_KEY);
}

IPAddress ipFromInfo(const esp_netif_ip_info_t &info)
{
    return IPAddress(info.ip.addr);
}

} // namespace

bool ETHClass::config(const char *hostname)
{
    // WLED configures the Ethernet itself; the hostname is the only thing
    // that can still be set from here.
    if (hostname) {
        return setHostname(hostname);
    }
    return true;
}

bool ETHClass::setHostname(const char *hostname)
{
    esp_netif_t *netif = ethNetif();
    if (!netif || !hostname) {
        return false;
    }
    return esp_netif_set_hostname(netif, hostname) == ESP_OK;
}

IPAddress ETHClass::localIP()
{
    esp_netif_t *netif = ethNetif();
    if (!netif) {
        return IPAddress();
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return IPAddress();
    }
    return ipFromInfo(info);
}

IPAddress ETHClass::subnetMask()
{
    esp_netif_t *netif = ethNetif();
    if (!netif) {
        return IPAddress();
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return IPAddress();
    }
    return ipFromInfo(info.netmask);
}

IPAddress ETHClass::gatewayIP()
{
    esp_netif_t *netif = ethNetif();
    if (!netif) {
        return IPAddress();
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return IPAddress();
    }
    return ipFromInfo(info.gw);
}

uint8_t ETHClass::macAddress(uint8_t *mac)
{
    esp_netif_t *netif = ethNetif();
    if (!netif || !mac) {
        return 0;
    }
    return esp_netif_get_mac(netif, mac) == ESP_OK ? 6 : 0;
}

bool ETHClass::linkUp()
{
    esp_netif_t *netif = ethNetif();
    if (!netif) {
        return false;
    }
    return esp_netif_is_running(netif);
}

// --- the WiFi stub ----------------------------------------------------------
//
// The P4 has no radio: the C6 coprocessor provides Wi-Fi over esp-hosted, and
// this build runs Ethernet only.  The Arduino libraries still ask for
// WiFi.localIP() (ESPAsyncWebServer compares the request's local address with
// the interface's), so the answer is the Ethernet address.

IPAddress WiFiClass::localIP()
{
    return ETH.localIP();
}

IPAddress WiFiClass::softAPIP()
{
    return IPAddress();
}

IPAddress WiFiClass::subnetMask()
{
    return ETH.subnetMask();
}

IPAddress WiFiClass::gatewayIP()
{
    return ETH.gatewayIP();
}

uint8_t WiFiClass::macAddress(uint8_t *mac)
{
    return ETH.macAddress(mac);
}

WiFiClass WiFi;
