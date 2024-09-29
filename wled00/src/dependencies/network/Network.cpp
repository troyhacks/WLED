#include "Network.h"

IPAddress NetworkClass::localIP()
{
  
#if defined(ARDUINO_ARCH_ESP32) // && defined(WLED_USE_ETHERNET) // TROYHACKS
  #ifdef ARDUINO_ARCH_ESP32P4
  esp_netif_ip_info_t ip_info;
  esp_netif_get_ip_info(esp_netif_get_default_netif(),&ip_info);
  // esp_netif_get_ip_info(ESP_IF_WIFI_STA,&ip_info);
  IPAddress localIP;
  char buf[32];
  sprintf(buf, IPSTR, IP2STR(&ip_info.ip));
  localIP = buf;
  if (localIP[0] != 0) {
    return localIP;
  }
  #else
  IPAddress localIP = ETH.localIP();
  if (localIP[0] != 0) {
    return localIP;
  }
  #endif
#endif
  // localIP = WiFi.localIP();
  // if (localIP[0] != 0) {
  //   return localIP;
  // }

  return INADDR_NONE;
}

IPAddress NetworkClass::subnetMask()
{
  #ifdef ARDUINO_ARCH_ESP32P4
  esp_netif_ip_info_t ip_info;
  esp_netif_get_ip_info(esp_netif_get_default_netif(),&ip_info);
  // esp_netif_get_ip_info(ESP_IF_WIFI_STA,&ip_info);
  IPAddress localIP;
  char buf[32];
  sprintf(buf, IPSTR, IP2STR(&ip_info.netmask));
  localIP = buf;
  if (localIP[0] != 0) {
    return localIP;
  }
  #else
  IPAddress localIP = ETH.localIP();
  if (localIP[0] != 0) {
    return localIP;
  }
  #endif
#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
  if (ETH.localIP()[0] != 0) {
    return ETH.subnetMask();
  }
#endif
  // if (WiFi.localIP()[0] != 0) {
  //   return WiFi.subnetMask();
  // }
  return IPAddress(255, 255, 255, 0);
}

IPAddress NetworkClass::gatewayIP()
{
  #ifdef ARDUINO_ARCH_ESP32P4
  esp_netif_ip_info_t ip_info;
  esp_netif_get_ip_info(esp_netif_get_default_netif(),&ip_info);
  // esp_netif_get_ip_info(ESP_IF_WIFI_STA,&ip_info);
  IPAddress localIP;
  char buf[32];
  sprintf(buf, IPSTR, IP2STR(&ip_info.gw));
  localIP = buf;
  if (localIP[0] != 0) {
    return localIP;
  }
  #else
  IPAddress localIP = ETH.localIP();
  if (localIP[0] != 0) {
    return localIP;
  }
  #endif
#if defined(ARDUINO_ARCH_ESP32P4) && defined(WLED_USE_ETHERNET)
  if (ETH.localIP()[0] != 0) {
      return ETH.gatewayIP();
  }
#endif
  // if (WiFi.localIP()[0] != 0) {
  //     return WiFi.gatewayIP();
  // }
  return INADDR_NONE;
}

void NetworkClass::localMAC(uint8_t* MAC) {

#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
  // ETH.macAddress(MAC); // Does not work because of missing ETHClass:: in ETH.ccp

  // Start work around
  String macString = ETH.macAddress();
  char macChar[18];
  char * octetEnd = macChar;

  strlcpy(macChar, macString.c_str(), 18);

  for (uint8_t i = 0; i < 6; i++) {
    MAC[i] = (uint8_t)strtol(octetEnd, &octetEnd, 16);
    octetEnd++;
  }
  // End work around

  for (uint8_t i = 0; i < 6; i++) {
    if (MAC[i] != 0x00) {
      return;
    }
  }
#endif
  // WiFi.macAddress(MAC);
  return;

}

bool NetworkClass::isConnected() {
  #if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
    return true;
  #else
    // return (WiFi.localIP()[0] != 0 && WiFi.status() == WL_CONNECTED);
  #endif
  return true;
}

bool NetworkClass::isEthernet() {
  #if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
    return (ETH.localIP()[0] != 0);
  #endif
    return false;
}

#ifdef ARDUINO_ARCH_ESP32
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
NetworkClass WL_Network;
#else
NetworkClass Network;
#endif
#else
NetworkClass Network;
#endif