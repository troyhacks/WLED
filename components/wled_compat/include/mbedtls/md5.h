/*
 * mbedtls/md5.h - the MD5 header the Arduino libraries expect.
 *
 * The companion of mbedtls/sha1.h: ESP-IDF 6.0 moved to mbedTLS 4,
 * where the legacy hash headers are private and the "_ret" variants
 * are gone.  The AsyncWebServer's basic authentication uses MD5 for the
 * digest.
 *
 * See pio-scripts/patch_mbedtls_legacy_hashes.py for the software MD5
 * being asked for: on a chip with a ROM MD5 mbedTLS 4 does not build
 * it, and the old API would not be there to call.
 */

#pragma once

#include <stddef.h>

#include "esp_idf_version.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)

// The private headers hide their members and their functions behind
// this: without it the members are called "private_total" and the
// functions are not declared at all.  With it, both are the plain
// names - and the layout is the same either way.
#ifndef MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#endif

#include "mbedtls/private/md5.h"

static inline int mbedtls_md5_starts_ret(mbedtls_md5_context *ctx) {
    return mbedtls_md5_starts(ctx);
}

static inline int mbedtls_md5_update_ret(mbedtls_md5_context *ctx,
                                          const unsigned char *input,
                                          size_t ilen) {
    return mbedtls_md5_update(ctx, input, ilen);
}

static inline int mbedtls_md5_finish_ret(mbedtls_md5_context *ctx,
                                          unsigned char output[16]) {
    return mbedtls_md5_finish(ctx, output);
}

#else

#include_next <mbedtls/md5.h>

#endif
