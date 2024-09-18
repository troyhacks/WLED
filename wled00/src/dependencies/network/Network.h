#ifdef ESP8266
  #include <ESP8266WiFi.h>
#else // ESP32
  #ifdef CONFIG_IDF_TARGET_ESP32P4
  #include <esp_wifi_remote.h>
  #else
  #include "WiFi.h"
  #endif
  #include <ETH.h>
#endif

#ifndef Network_h
#define Network_h

class NetworkClass
{
public:
  IPAddress localIP();
  IPAddress subnetMask();
  IPAddress gatewayIP();
  void localMAC(uint8_t* MAC);
  bool isConnected();
  bool isEthernet();
};

#ifdef ARDUINO_ARCH_ESP32
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
extern NetworkClass WL_Network;
#else
extern NetworkClass Network;
#endif
#else
extern NetworkClass Network;
#endif

#endif