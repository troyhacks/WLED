// ETH.h - the Arduino Ethernet class.
//
// The P4's Ethernet is an ESP32 EMAC with an IP101 PHY, and WLED sets it up
// itself in wled00/wled.cpp with esp_eth/esp_netif.  This class is the
// Arduino-shaped view of that netif - it looks the netif up rather than
// owning it.
//
// The enumerations are the Arduino core's (libraries/Ethernet/src/ETH.h): the
// core has them, and WLED's own EthernetConfig uses them.  The core's
// versions are conditional on the IDF's Kconfig, so they are repeated here
// with the same conditions.

#pragma once

#include "Arduino.h"
#include "IPAddress.h"

extern "C" {
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_netif.h"
#include "sdkconfig.h"
}

// The clock mode, the way the Arduino core has it.
#if CONFIG_ETH_USE_ESP32_EMAC
#if defined(CONFIG_IDF_TARGET_ESP32)
typedef enum {
    ETH_CLOCK_GPIO0_IN,
    ETH_CLOCK_GPIO0_OUT,
    ETH_CLOCK_GPIO16_OUT,
    ETH_CLOCK_GPIO17_OUT
} eth_clock_mode_t;
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
typedef emac_rmii_clock_mode_t eth_clock_mode_t;
#endif

// The PHY, the way the Arduino core has it.
typedef enum {
#if CONFIG_ETH_USE_ESP32_EMAC
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    ETH_PHY_GENERIC,
#define ETH_PHY_JL1101 ETH_PHY_GENERIC
#endif
    ETH_PHY_LAN8720,
    ETH_PHY_TLK110,
    ETH_PHY_RTL8201,
    ETH_PHY_DP83848,
    ETH_PHY_KSZ8041,
    ETH_PHY_KSZ8081,
#endif /* CONFIG_ETH_USE_ESP32_EMAC */
#if CONFIG_ETH_SPI_ETHERNET_DM9051
    ETH_PHY_DM9051,
#endif
#if CONFIG_ETH_SPI_ETHERNET_W5500
    ETH_PHY_W5500,
#endif
#if CONFIG_ETH_SPI_ETHERNET_KSZ8851SNL
    ETH_PHY_KSZ8851,
#endif
    ETH_PHY_MAX
} eth_phy_type_t;

#ifndef ETH_PHY_ADDR_AUTO
#define ETH_PHY_ADDR_AUTO ESP_ETH_PHY_ADDR_AUTO
#endif
#ifndef ETH_SPI_SUPPORTS_CUSTOM
#define ETH_SPI_SUPPORTS_CUSTOM 1
#endif
#ifndef ETH_SPI_SUPPORTS_NO_IRQ
#define ETH_SPI_SUPPORTS_NO_IRQ 1
#endif
#ifndef ETH_PHY_SPI_FREQ_MHZ
#define ETH_PHY_SPI_FREQ_MHZ 20
#endif
#endif // CONFIG_ETH_USE_ESP32_EMAC

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
