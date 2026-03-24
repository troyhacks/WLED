#include "wled.h"
#include "Network.h"
#include "esp_netif.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "mdns.h"
#include "esp_netif_net_stack.h"
#include "esp_mac.h"

// lwIP source interface — set by localIP() so UDP sockets can bind to the right netif.
static struct netif* sender_netif = nullptr;

IPAddress NetworkClass::localIP() {
  esp_netif_ip_info_t ip_info;
  esp_netif_t* wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_t* eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");

  uint32_t wifi_metric = 0, eth_metric = 0;
  if (wifi_netif) wifi_metric = esp_netif_get_route_prio(wifi_netif);
  if (eth_netif) eth_metric = esp_netif_get_route_prio(eth_netif);

  // Determine which interface to try first based on priority
  esp_netif_t* primary = (eth_metric >= wifi_metric) ? eth_netif : wifi_netif;
  esp_netif_t* secondary = (eth_metric >= wifi_metric) ? wifi_netif : eth_netif;
  
  esp_netif_set_default_netif(primary);

  // Try primary interface first
  if (primary && esp_netif_get_ip_info(primary, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
    sender_netif = static_cast<struct netif*>(esp_netif_get_netif_impl(primary));
    mdns_netif_action(primary, MDNS_EVENT_DISABLE_IP6);
    mdns_netif_action(secondary, MDNS_EVENT_DISABLE_IP4);
    mdns_netif_action(secondary, MDNS_EVENT_DISABLE_IP6);
    return IPAddress(ip_info.ip.addr);
  }

  // Fall back to secondary interface
  if (secondary && esp_netif_get_ip_info(secondary, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
    sender_netif = static_cast<struct netif*>(esp_netif_get_netif_impl(secondary));
    mdns_netif_action(secondary, MDNS_EVENT_DISABLE_IP6);
    mdns_netif_action(primary, MDNS_EVENT_DISABLE_IP4);
    mdns_netif_action(primary, MDNS_EVENT_DISABLE_IP6);
    return IPAddress(ip_info.ip.addr);
  }

  return INADDR_NONE;
}

IPAddress NetworkClass::getWiFiIP() {
  esp_netif_ip_info_t ip_info;
  esp_netif_t* wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

  if (wifi_netif && esp_netif_get_ip_info(wifi_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
    return IPAddress(ip_info.ip.addr);
  } else { 
    return INADDR_NONE;
  }
}

IPAddress NetworkClass::getEthernetIP() {
  esp_netif_ip_info_t ip_info;
  esp_netif_t* eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");

  if (eth_netif && esp_netif_get_ip_info(eth_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
    return IPAddress(ip_info.ip.addr);
  } else {
    return INADDR_NONE;
  }
}

IPAddress NetworkClass::softAPIP() {
  esp_netif_ip_info_t ip_info;
  esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
    return IPAddress(ip_info.ip.addr);
  }
  return IPAddress(192, 168, 4, 1);  // Fallback to default
}

IPAddress NetworkClass::subnetMask() {
  esp_netif_ip_info_t ip_info;
  esp_netif_t* netif = esp_netif_get_default_netif();
  if (netif) {
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
      return IPAddress(ip_info.netmask.addr);
    }
  }
  return IPAddress(0, 0, 0, 0);
}

IPAddress NetworkClass::gatewayIP() {
  esp_netif_ip_info_t ip_info;
  esp_netif_t* netif = esp_netif_get_default_netif();
  if (netif) {
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
      return IPAddress(ip_info.gw.addr);
    }
  }
  return INADDR_NONE;
}

void NetworkClass::localMAC(uint8_t* MAC) {
  memset(MAC, 0, 6);
  esp_netif_t* default_netif = esp_netif_get_default_netif();
  if (default_netif == NULL) {
    return; // No default interface is active
  }
  esp_netif_t* wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_t* eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
  if (default_netif == wifi_netif) {
    esp_wifi_get_mac(WIFI_IF_STA, MAC);
  } else {
    esp_netif_get_mac(eth_netif, MAC);
  }
  return;
}

bool NetworkClass::isConnected() {
  esp_netif_t* netif = esp_netif_get_default_netif();
  if (netif == NULL) {
    return false;
  }
  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
    return (ip_info.ip.addr != 0) && (ip_info.ip.addr != IPADDR_BROADCAST);
  }
  return false;
}

IPAddress NetworkClass::hostByName(const char* hostname) {
  // Note: Arduino IPAddress is IPv4 only. This function will return
  // INADDR_NONE if the hostname resolves only to an IPv6 address.
  // Use getaddrinfo to perform the DNS lookup for ANY address family
  struct addrinfo hints = {
      .ai_family = AF_UNSPEC, // Allow either IPv4 or IPv6
      .ai_socktype = SOCK_STREAM,
  };
  struct addrinfo* res;
  if (getaddrinfo(hostname, NULL, &hints, &res) == 0 && res != NULL) {
    IPAddress result = INADDR_NONE;
    // Check the address family of the first result
    if (res->ai_family == AF_INET) {
      // It's an IPv4 address, which IPAddress can handle.
      struct in_addr* addr = &((struct sockaddr_in*)res->ai_addr)->sin_addr;
      result = IPAddress(addr->s_addr);
    } else if (res->ai_family == AF_INET6) {
      // It's an IPv6 address. The Arduino IPAddress object cannot store it.
      // You could log this if needed.
      ESP_LOGE("Network", "Hostname '%s' resolved to an IPv6 address, which is not supported.", hostname);
    }
    freeaddrinfo(res);
    return result;
  }
  // If we get here, the lookup failed
  return INADDR_NONE;
}

String NetworkClass::format_mac_address(const uint8_t* mac) {
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(mac_str);
}

esp_err_t NetworkClass::get_hardware_mac_address(uint8_t* mac_addr) {

  // This gets the MAC from the hardware, before any network service init happens.
  esp_err_t err = ESP_FAIL;
  
  #if defined(WLED_USE_ETHERNET) 
  err = esp_read_mac(mac_addr, ESP_MAC_ETH);
  if (err == ESP_OK) {
    return ESP_OK;
  }
  #endif
  err = esp_read_mac(mac_addr, ESP_MAC_WIFI_STA);
  if (err == ESP_OK) {
    return ESP_OK;
  }
  USER_PRINTLN("Failed to read MAC");
  return err;
}

String NetworkClass::getEscapedMac() {
  uint8_t mac_addr[6];
  this->get_hardware_mac_address(mac_addr);
  String formatted_mac = this->format_mac_address(mac_addr);
  formatted_mac.replace(":", "");
  formatted_mac.toLowerCase();
  return formatted_mac;
}

bool NetworkClass::isEthernet() {
  esp_netif_t* default_netif = esp_netif_get_default_netif();
  if (default_netif == NULL) {
    return false; // No default interface is activeC
  }
  esp_netif_t* wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_t* eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
  if (default_netif == wifi_netif) {
    return false;
  } else if (default_netif == eth_netif) {
    return true;
  }
  return false;
}

bool NetworkClass::isWiFi() {
  esp_netif_t* default_netif = esp_netif_get_default_netif();
  if (default_netif == NULL) {
    return false; // No default interface is active
  }
  esp_netif_t* wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_t* eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
  if (default_netif == wifi_netif) {
    return true;
  } else if (default_netif == eth_netif) {
    return false;
  }
  return false;
}

bool NetworkClass::setHostname(const char* hostname) {

  esp_err_t err = ESP_OK;

  #if !defined(WLED_USE_ETHERNET_ONLY)
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif == NULL) {
    ESP_LOGE("NetworkClass", "Failed to get WiFi network interface handle.");
    err = ESP_ERR_INVALID_ARG;
  }
  err = esp_netif_set_hostname(netif, hostname);
  if (err != ESP_OK) {
    ESP_LOGE("NetworkClass", "Failed to set WiFi hostname, error: %s", esp_err_to_name(err));
  }
  ESP_LOGI("NetworkClass", "WiFi hostname set to '%s'", hostname);
  #elif defined(WLED_USE_ETHERNET)
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
  if (netif == NULL) {
    ESP_LOGE("NetworkClass", "Failed to get ETH network interface handle.");
    err = ESP_ERR_INVALID_ARG;
  }
  err = esp_netif_set_hostname(netif, hostname);
  if (err != ESP_OK) {
    ESP_LOGE("NetworkClass", "Failed to set Ethernet hostname, error: %s", esp_err_to_name(err));
  }
  ESP_LOGI("NetworkClass", "Ethernet hostname set to '%s'", hostname);
  #endif
  return err == ESP_OK ? true : false;
}

#ifdef ARDUINO_ARCH_ESP32
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0) && !defined(WLED_IDF_BUILD)
NetworkClass WL_Network;
#else
NetworkClass Network;
#endif
#else
NetworkClass Network;
#endif