// arduino_events.cpp - the Arduino core's network events.
//
// The Arduino core runs the network events through its own event system and
// calls a global WiFiEvent() with an arduino_event_id_t.  WLED has such a
// function (wled00/network.cpp) and it does the board's Ethernet bookkeeping:
// the static address, the hostname, the reconnect.
//
// Without the core there is nobody to call it, so this does what the core
// did: listen for ESP-IDF's Ethernet and IP events, translate them into the
// Arduino's event identifiers, and hand them over.

#include "Arduino.h"
#include "WiFiEvent.h"

extern "C" {
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_com.h"
#include "esp_log.h"
}

static const char *TAG = "wled_events";

// Whether the Ethernet link is up.  The Arduino core's ETH.linkUp() answered
// this from the driver; here the event handler keeps it.
static bool s_linkUp = false;

bool wledEthernetLinkUp()
{
    return s_linkUp;
}

// The application's event handler.  The Arduino core calls a function with
// this name; WLED has one.  It is weak, so a build without one still links.
extern void WiFiEvent(arduino_event_id_t event) __attribute__((weak));

static void IRAM_ATTR ethEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    arduino_event_id_t event = ARDUINO_EVENT_NONE;
    switch (id) {
    case ETHERNET_EVENT_START:
        event = ARDUINO_EVENT_ETH_START;
        break;
    case ETHERNET_EVENT_STOP:
        event = ARDUINO_EVENT_ETH_STOP;
        break;
    case ETHERNET_EVENT_CONNECTED:
        event = ARDUINO_EVENT_ETH_CONNECTED;
        s_linkUp = true;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        event = ARDUINO_EVENT_ETH_DISCONNECTED;
        s_linkUp = false;
        break;
    default:
        return;
    }
    ESP_LOGD(TAG, "Ethernet event %d", (int)event);
    if (WiFiEvent) {
        WiFiEvent(event);
    }
}

static void IRAM_ATTR ipEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    arduino_event_id_t event = ARDUINO_EVENT_NONE;
    switch (id) {
    case IP_EVENT_ETH_GOT_IP:
        event = ARDUINO_EVENT_ETH_GOT_IP;
        break;
    case IP_EVENT_ETH_LOST_IP:
        event = ARDUINO_EVENT_ETH_LOST_IP;
        break;
    default:
        return;
    }
    if (WiFiEvent) {
        WiFiEvent(event);
    }
}

void wledNetworkEventsBegin()
{
    static bool started = false;
    if (started) {
        return;
    }
    started = true;

    esp_event_loop_create_default();
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, ethEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ipEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, ipEventHandler, nullptr);
}
