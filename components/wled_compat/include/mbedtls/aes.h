/*
 * mbedtls/aes.h - the AES header the Arduino layer expects.
 *
 * The companion of mbedtls/sha1.h and mbedtls/md5.h: ESP-IDF 6.0 moved to
 * mbedTLS 4, where the legacy crypto headers are private and the API lives
 * in mbedtls/private/aes.h.  The Update class uses it to decrypt the
 * flash-encrypted image the OTA writes.
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

#include "mbedtls/private/aes.h"

#else

#include_next <mbedtls/aes.h>

#endif
