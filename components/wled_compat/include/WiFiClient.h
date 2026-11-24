// WiFiClient.h - the Arduino core's WiFi client.
//
// The core has it as a header of its own (libraries/WiFi/src/WiFiClient.h),
// and it is just the network client under another name.

#pragma once

#include "NetworkClient.h"
typedef NetworkClient WiFiClient;
