#include "ota_sha256.h"

#include <string.h>

static const uint32_t g_sha256_round_constants[64] =
{
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
    0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
    0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
    0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
    0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

static uint32_t ota_sha256_rotate_right(uint32_t value, uint32_t bits)
{
    return (value >> bits) | (value << (32U - bits));
}

static uint32_t ota_sha256_load_be32(const uint8_t * p_data)
{
    return ((uint32_t) p_data[0] << 24U) |
           ((uint32_t) p_data[1] << 16U) |
           ((uint32_t) p_data[2] << 8U) |
           (uint32_t) p_data[3];
}

static void ota_sha256_transform(ota_sha256_t * p_context, const uint8_t block[64])
{
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;

    for (uint32_t i = 0U; i < 16U; i++)
    {
        words[i] = ota_sha256_load_be32(&block[i * 4U]);
    }
    for (uint32_t i = 16U; i < 64U; i++)
    {
        uint32_t s0 = ota_sha256_rotate_right(words[i - 15U], 7U) ^
                      ota_sha256_rotate_right(words[i - 15U], 18U) ^
                      (words[i - 15U] >> 3U);
        uint32_t s1 = ota_sha256_rotate_right(words[i - 2U], 17U) ^
                      ota_sha256_rotate_right(words[i - 2U], 19U) ^
                      (words[i - 2U] >> 10U);
        words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
    }

    a = p_context->state[0];
    b = p_context->state[1];
    c = p_context->state[2];
    d = p_context->state[3];
    e = p_context->state[4];
    f = p_context->state[5];
    g = p_context->state[6];
    h = p_context->state[7];

    for (uint32_t i = 0U; i < 64U; i++)
    {
        uint32_t sum1 = ota_sha256_rotate_right(e, 6U) ^
                        ota_sha256_rotate_right(e, 11U) ^
                        ota_sha256_rotate_right(e, 25U);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temporary1 = h + sum1 + choose + g_sha256_round_constants[i] + words[i];
        uint32_t sum0 = ota_sha256_rotate_right(a, 2U) ^
                        ota_sha256_rotate_right(a, 13U) ^
                        ota_sha256_rotate_right(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    p_context->state[0] += a;
    p_context->state[1] += b;
    p_context->state[2] += c;
    p_context->state[3] += d;
    p_context->state[4] += e;
    p_context->state[5] += f;
    p_context->state[6] += g;
    p_context->state[7] += h;
}

void ota_sha256_init(ota_sha256_t * p_context)
{
    static const uint32_t initial_state[8] =
    {
        0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
        0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
    };

    if (NULL == p_context)
    {
        return;
    }
    memcpy(p_context->state, initial_state, sizeof(initial_state));
    p_context->total_bytes = 0U;
    p_context->block_bytes = 0U;
    memset(p_context->block, 0, sizeof(p_context->block));
}

void ota_sha256_update(ota_sha256_t * p_context, const uint8_t * p_data, size_t length)
{
    if ((NULL == p_context) || ((NULL == p_data) && (length > 0U)))
    {
        return;
    }

    p_context->total_bytes += length;
    while (length > 0U)
    {
        size_t copy_length = sizeof(p_context->block) - p_context->block_bytes;
        if (copy_length > length)
        {
            copy_length = length;
        }
        memcpy(&p_context->block[p_context->block_bytes], p_data, copy_length);
        p_context->block_bytes += copy_length;
        p_data += copy_length;
        length -= copy_length;
        if (p_context->block_bytes == sizeof(p_context->block))
        {
            ota_sha256_transform(p_context, p_context->block);
            p_context->block_bytes = 0U;
        }
    }
}

void ota_sha256_final(ota_sha256_t * p_context, uint8_t digest[32])
{
    uint64_t total_bits;

    if ((NULL == p_context) || (NULL == digest))
    {
        return;
    }

    total_bits = p_context->total_bytes * 8U;
    p_context->block[p_context->block_bytes++] = 0x80U;
    if (p_context->block_bytes > 56U)
    {
        while (p_context->block_bytes < sizeof(p_context->block))
        {
            p_context->block[p_context->block_bytes++] = 0U;
        }
        ota_sha256_transform(p_context, p_context->block);
        p_context->block_bytes = 0U;
    }
    while (p_context->block_bytes < 56U)
    {
        p_context->block[p_context->block_bytes++] = 0U;
    }
    for (uint32_t i = 0U; i < 8U; i++)
    {
        p_context->block[63U - i] = (uint8_t) (total_bits >> (i * 8U));
    }
    ota_sha256_transform(p_context, p_context->block);

    for (uint32_t i = 0U; i < 8U; i++)
    {
        digest[i * 4U] = (uint8_t) (p_context->state[i] >> 24U);
        digest[i * 4U + 1U] = (uint8_t) (p_context->state[i] >> 16U);
        digest[i * 4U + 2U] = (uint8_t) (p_context->state[i] >> 8U);
        digest[i * 4U + 3U] = (uint8_t) p_context->state[i];
    }
    memset(p_context, 0, sizeof(*p_context));
}
