#ifndef OTA_SHA256_H_
#define OTA_SHA256_H_

#include <stddef.h>
#include <stdint.h>

typedef struct st_ota_sha256
{
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_bytes;
} ota_sha256_t;

void ota_sha256_init(ota_sha256_t * p_context);
void ota_sha256_update(ota_sha256_t * p_context, const uint8_t * p_data, size_t length);
void ota_sha256_final(ota_sha256_t * p_context, uint8_t digest[32]);

#endif /* OTA_SHA256_H_ */
