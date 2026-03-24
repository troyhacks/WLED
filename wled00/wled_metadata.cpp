// wled_metadata.cpp — Firmware metadata definitions.
#include "wled_metadata.h"
#include "wled.h"

// ── Metadata struct in dedicated flash section ────────────────────────────────
const wled_metadata_t WLED_BUILD_DESCRIPTION
    __attribute__((section(".wled_metadata"), used)) = {
    .magic               = WLED_METADATA_MAGIC,
    .desc_version        = WLED_METADATA_DESC_VERSION,
    .wled_version        = VERSION,
    .version_string      = TOSTRING(VERSION),
    .release_name        = TOSTRING(WLED_RELEASE_NAME),
    .hash                = "",
    .safe_update_version = 0,
};

// ── Mutable version/release strings (some callers take char*) ─────────────────
char versionString[16] = TOSTRING(VERSION);
char releaseString[32] = TOSTRING(WLED_RELEASE_NAME);

// ── String constants ───────────────────────────────────────────────────────────
const char productString[] = "WLED-MM";
const char brandString[]   = "WLEDMM";

// repoString declared in wled.h as extern const __FlashStringHelper*
const __FlashStringHelper* repoString = (const __FlashStringHelper*)"troyhacks/WLED";

// ── findWledMetadata ───────────────────────────────────────────────────────────
const wled_metadata_t* findWledMetadata() {
    return &WLED_BUILD_DESCRIPTION;
}

// ── shouldAllowOTA ─────────────────────────────────────────────────────────────
bool shouldAllowOTA(uint32_t candidateVersion) {
    if (WLED_BUILD_DESCRIPTION.safe_update_version == 0) return true;
    return candidateVersion >= WLED_BUILD_DESCRIPTION.safe_update_version;
}
