/*
 * mbedtls/sha1.h - the SHA-1 header the Arduino libraries expect.
 *
 * ESP-IDF 6.0 moved to mbedTLS 4, where the legacy hash headers are
 * gone: mbedtls/sha1.h is not there any more, and the API lives in
 * mbedtls/private/sha1.h.  The API itself is unchanged, apart from the
 * "_ret" variants - they are gone, because the plain functions return
 * the error code now.  Those are put back here, so the code that uses
 * them (the AsyncWebServer's WebSocket handshake) does not have to know.
 *
 * The implementation is only built when the software SHA-1 is asked
 * for - see pio-scripts/patch_mbedtls_legacy_hashes.py, which does
 * that: on a chip with a SHA accelerator mbedTLS 4 does not build it,
 * and the old API would not be there to call.
 *
 * This header is found before the IDF's, because the IDF does not have
 * one of this name any more.  On an older IDF the IDF's own header is
 * used.
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

#include "mbedtls/private/sha1.h"

static inline int mbedtls_sha1_starts_ret(mbedtls_sha1_context *ctx) {
    return mbedtls_sha1_starts(ctx);
}

static inline int mbedtls_sha1_update_ret(mbedtls_sha1_context *ctx,
                                           const unsigned char *input,
                                           size_t ilen) {
    return mbedtls_sha1_update(ctx, input, ilen);
}

static inline int mbedtls_sha1_finish_ret(mbedtls_sha1_context *ctx,
                                           unsigned char output[20]) {
    return mbedtls_sha1_finish(ctx, output);
}

#else

#include_next <mbedtls/sha1.h>

#endif
