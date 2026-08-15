#ifndef OTA_BOOT_RECORD_H_
#define OTA_BOOT_RECORD_H_

#include <stdint.h>

#define OTA_BOOT_RECORD_MAGIC              (0x3141544FU) /* "OTA1" */
#define OTA_BOOT_RECORD_SCHEMA             (1U)
#define OTA_BOOT_RECORD_BYTES              (256U)
#define OTA_BOOT_RECORD_DEPLOYMENT_ID_MAX  (32U)
#define OTA_BOOT_RECORD_VERSION_MAX        (32U)

typedef enum e_ota_boot_state
{
    OTA_BOOT_STATE_EMPTY = 0,
    OTA_BOOT_STATE_DOWNLOADING = 1,
    OTA_BOOT_STATE_PENDING_TRIAL = 2,
    OTA_BOOT_STATE_CONFIRMED = 3,
    OTA_BOOT_STATE_ROLLBACK = 4,
} ota_boot_state_t;

/*
 * Shared application/bootloader record.  Each copy occupies four 64-byte
 * RA6M5 Data Flash erase blocks.  The CRC covers every byte except crc32.
 */
typedef struct st_ota_boot_record
{
    uint32_t magic;
    uint16_t schema;
    uint8_t state;
    uint8_t target_bank;
    uint32_t generation;
    uint32_t total_length;
    uint32_t received_length;
    uint32_t trial_boots;
    char deployment_id[OTA_BOOT_RECORD_DEPLOYMENT_ID_MAX];
    char target_version[OTA_BOOT_RECORD_VERSION_MAX];
    uint8_t image_sha256[32];
    uint8_t reserved[132];
    uint32_t crc32;
} ota_boot_record_t;

typedef char ota_boot_record_size_must_be_256[
    (sizeof(ota_boot_record_t) == OTA_BOOT_RECORD_BYTES) ? 1 : -1
];

#endif /* OTA_BOOT_RECORD_H_ */
