// wled_metadata.h — Build-time firmware metadata embedded in flash.
// Provides version/release strings and OTA compatibility checking.
#pragma once

#include <stdint.h>

#define WLED_METADATA_MAGIC        0x574C4544UL  // 'WLED'
#define WLED_METADATA_DESC_VERSION 1

struct wled_metadata_t {
    uint32_t magic;               // WLED_METADATA_MAGIC
    uint16_t desc_version;        // struct layout version
    uint32_t wled_version;        // VERSION integer (e.g. 2603101)
    char     version_string[16];  // e.g. "2603101"
    char     release_name[32];    // e.g. "ESP32-P4_IDF"
    char     hash[16];            // short git hash (if available)
    uint32_t safe_update_version; // minimum VERSION that can OTA-update to this build
};

// The metadata struct is placed in a dedicated flash section.
extern const wled_metadata_t WLED_BUILD_DESCRIPTION;

// Version/release as mutable char arrays (some callers take char*).
// Defined in wled_metadata.cpp from the struct values at startup.
extern char versionString[16];
extern char releaseString[32];

// External string constants
extern const char productString[];
extern const char brandString[];

// ── Functions ─────────────────────────────────────────────────────────────────
const wled_metadata_t* findWledMetadata();
bool shouldAllowOTA(uint32_t candidateVersion);
