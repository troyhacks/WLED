/*
 * mbedtls_legacy_hashes.c - SHA-1 and MD5 for the legacy mbedTLS API.
 *
 * ESP-IDF 6.0 moved to mbedTLS 4, where the legacy hash headers are
 * gone - and on a chip with a SHA accelerator or a ROM MD5 the
 * software implementations are not built either, because the hardware
 * does the hashing.  So mbedtls_sha1_init() and mbedtls_md5_init()
 * are not there to call.
 *
 * The AsyncWebServer calls them: the WebSocket handshake takes the
 * SHA-1 of the client's key and the GUID, and the basic authentication
 * takes an MD5 of the request.
 *
 * Both are implemented here, on the mbedTLS context structures (which
 * are the classic state layouts), so the old API keeps working.  The
 * algorithms are the ones everybody has: SHA-1 from FIPS 180-1 and MD5
 * from RFC 1321.
 *
 * The files that include these are components/wled_compat/include/
 * mbedtls/sha1.h and mbedtls/md5.h - they are the headers the
 * libraries include, and on an older IDF the mbedTLS ones are used
 * instead of this.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
#include "mbedtls/private/md5.h"

/* ------------------------------------------------------------------ */
/* SHA-1 (FIPS 180-1)                                                 */
/* ------------------------------------------------------------------ */

#define SHA1_BLOCK_SIZE 64

static uint32_t rol32(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_process_block(mbedtls_sha1_context *ctx, const unsigned char *block) {
    uint32_t w[80];
    uint32_t a, b, c, d, e, f, k, temp;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        temp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = temp;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

void mbedtls_sha1_init(mbedtls_sha1_context *ctx) {
    memset(ctx, 0, sizeof(mbedtls_sha1_context));
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
}

void mbedtls_sha1_free(mbedtls_sha1_context *ctx) {
    if (ctx) {
        memset(ctx, 0, sizeof(mbedtls_sha1_context));
    }
}

int mbedtls_sha1_starts(mbedtls_sha1_context *ctx) {
    if (!ctx) {
        return -1;
    }
    ctx->total[0] = 0;
    ctx->total[1] = 0;
    return 0;
}

int mbedtls_sha1_update(mbedtls_sha1_context *ctx, const unsigned char *input, size_t ilen) {
    size_t fill;
    uint32_t left;

    if (!ctx || (!input && ilen)) {
        return -1;
    }
    if (ilen == 0) {
        return 0;
    }

    left = (ctx->total[0] & 0x3F) + (ctx->total[1] << 26);
    fill = SHA1_BLOCK_SIZE - left;
    ctx->total[0] += (uint32_t)ilen;
    if (ctx->total[0] < (uint32_t)ilen) {
        ctx->total[1]++;
    }
    ctx->total[1] += (uint32_t)(ilen >> 29);

    if (left && ilen >= fill) {
        memcpy(ctx->buffer + left, input, fill);
        sha1_process_block(ctx, ctx->buffer);
        input += fill;
        ilen -= fill;
        left = 0;
    }

    while (ilen >= SHA1_BLOCK_SIZE) {
        sha1_process_block(ctx, input);
        input += SHA1_BLOCK_SIZE;
        ilen -= SHA1_BLOCK_SIZE;
    }

    if (ilen) {
        memcpy(ctx->buffer + left, input, ilen);
    }
    return 0;
}

int mbedtls_sha1_finish(mbedtls_sha1_context *ctx, unsigned char output[20]) {
    uint32_t high, low;
    size_t last, pad;
    // The padding is the 0x80, the zeroes and the 8 length bytes: 72 bytes
    // at the most (when the message ends 56 bytes into a block, the length
    // needs a block of its own).
    unsigned char padding[SHA1_BLOCK_SIZE + 8];
    int i;

    if (!ctx || !output) {
        return -1;
    }

    high = (ctx->total[0] >> 29) | (ctx->total[1] << 3);
    low = (ctx->total[0] << 3);

    memset(padding, 0, sizeof(padding));
    padding[0] = 0x80;

    last = (size_t)(ctx->total[0] & 0x3F);
    pad = (last < 56) ? (56 - last) : (120 - last);
    padding[pad + 0] = (unsigned char)(high >> 24);
    padding[pad + 1] = (unsigned char)(high >> 16);
    padding[pad + 2] = (unsigned char)(high >> 8);
    padding[pad + 3] = (unsigned char)(high);
    padding[pad + 4] = (unsigned char)(low >> 24);
    padding[pad + 5] = (unsigned char)(low >> 16);
    padding[pad + 6] = (unsigned char)(low >> 8);
    padding[pad + 7] = (unsigned char)(low);
    pad += 8;

    mbedtls_sha1_update(ctx, padding, pad);

    for (i = 0; i < 5; i++) {
        output[i * 4 + 0] = (unsigned char)(ctx->state[i] >> 24);
        output[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
        output[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
        output[i * 4 + 3] = (unsigned char)(ctx->state[i]);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* MD5 (RFC 1321)                                                     */
/* ------------------------------------------------------------------ */

#define MD5_BLOCK_SIZE 64

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

static const unsigned char md5_shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static void md5_process_block(mbedtls_md5_context *ctx, const unsigned char *block) {
    uint32_t m[16];
    uint32_t a, b, c, d, f, temp, g;
    int i;

    for (i = 0; i < 16; i++) {
        m[i] = ((uint32_t)block[i * 4]) | ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];

    for (i = 0; i < 64; i++) {
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | (~d));
            g = (7 * i) % 16;
        }
        temp = d;
        d = c;
        c = b;
        f = f + a + md5_k[i] + m[g];
        b = b + rol32(f, md5_shift[i]);
        a = temp;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

void mbedtls_md5_init(mbedtls_md5_context *ctx) {
    memset(ctx, 0, sizeof(mbedtls_md5_context));
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

void mbedtls_md5_free(mbedtls_md5_context *ctx) {
    if (ctx) {
        memset(ctx, 0, sizeof(mbedtls_md5_context));
    }
}

int mbedtls_md5_starts(mbedtls_md5_context *ctx) {
    if (!ctx) {
        return -1;
    }
    ctx->total[0] = 0;
    ctx->total[1] = 0;
    return 0;
}

int mbedtls_md5_update(mbedtls_md5_context *ctx, const unsigned char *input, size_t ilen) {
    size_t fill;
    uint32_t left;

    if (!ctx || (!input && ilen)) {
        return -1;
    }
    if (ilen == 0) {
        return 0;
    }

    left = (ctx->total[0] & 0x3F) + (ctx->total[1] << 26);
    fill = MD5_BLOCK_SIZE - left;
    ctx->total[0] += (uint32_t)ilen;
    if (ctx->total[0] < (uint32_t)ilen) {
        ctx->total[1]++;
    }
    ctx->total[1] += (uint32_t)(ilen >> 29);

    if (left && ilen >= fill) {
        memcpy(ctx->buffer + left, input, fill);
        md5_process_block(ctx, ctx->buffer);
        input += fill;
        ilen -= fill;
        left = 0;
    }

    while (ilen >= MD5_BLOCK_SIZE) {
        md5_process_block(ctx, input);
        input += MD5_BLOCK_SIZE;
        ilen -= MD5_BLOCK_SIZE;
    }

    if (ilen) {
        memcpy(ctx->buffer + left, input, ilen);
    }
    return 0;
}

int mbedtls_md5_finish(mbedtls_md5_context *ctx, unsigned char output[16]) {
    uint32_t high, low;
    size_t last, pad;
    // 72 bytes at the most - see the SHA-1 above.
    unsigned char padding[MD5_BLOCK_SIZE + 8];
    int i;

    if (!ctx || !output) {
        return -1;
    }

    high = (ctx->total[0] >> 29) | (ctx->total[1] << 3);
    low = ctx->total[0] << 3;

    memset(padding, 0, sizeof(padding));
    padding[0] = 0x80;

    last = (size_t)(ctx->total[0] & 0x3F);
    pad = (last < 56) ? (56 - last) : (120 - last);
    padding[pad + 0] = (unsigned char)(low);
    padding[pad + 1] = (unsigned char)(low >> 8);
    padding[pad + 2] = (unsigned char)(low >> 16);
    padding[pad + 3] = (unsigned char)(low >> 24);
    padding[pad + 4] = (unsigned char)(high);
    padding[pad + 5] = (unsigned char)(high >> 8);
    padding[pad + 6] = (unsigned char)(high >> 16);
    padding[pad + 7] = (unsigned char)(high >> 24);
    pad += 8;

    mbedtls_md5_update(ctx, padding, pad);

    for (i = 0; i < 4; i++) {
        output[i * 4 + 0] = (unsigned char)(ctx->state[i]);
        output[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 8);
        output[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 16);
        output[i * 4 + 3] = (unsigned char)(ctx->state[i] >> 24);
    }
    return 0;
}

#endif /* ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) */
