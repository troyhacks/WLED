/*
 * esp_interface.h - the interface enum the Arduino API expects.
 *
 * ESP-IDF had this header in esp_hw_support up to v5.5, and the Arduino
 * core's own ESPmDNS.h includes it (for MDNSResponder::enableWorkstation).
 *  ESP-IDF v6.0 took the whole "netif driver" interface out with the
 *  esp_netif rework, and the header is gone.
 *
 * Nothing in the IDF uses the type any more, and nothing in this firmware
 * calls enableWorkstation() - it is here so the vendored Arduino sources
 * keep their original shape and stay comparable with the Arduino build.
 *
 * The values are the ones the IDF used, so a ported source sees the same
 * numbers it saw before.
 */

#ifndef __ESP_INTERFACE_H__
#define __ESP_INTERFACE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_IF_WIFI_STA = 0,     /**< Station interface */
    ESP_IF_WIFI_AP,          /**< Soft-AP interface */
    ESP_IF_WIFI_NAN,         /**< NAN interface */
    ESP_IF_ETH,              /**< Ethernet interface */
    ESP_IF_MAX
} esp_interface_t;

#ifdef __cplusplus
}
#endif

#endif /* __ESP_INTERFACE_H__ */
