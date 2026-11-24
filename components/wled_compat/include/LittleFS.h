// LittleFS.h - the Arduino filesystem, on ESP-IDF's LittleFS.
//
// The partition is labelled "spiffs": that is the label the Arduino build
// mounts, and the label is what finds the partition, so keeping it means a
// board can be reflashed with either build without losing its data.

#pragma once

#include "FS.h"

class LittleFS : public FS {
public:
    bool begin(bool formatOnFail = false);
};

extern LittleFS LittleFS;
