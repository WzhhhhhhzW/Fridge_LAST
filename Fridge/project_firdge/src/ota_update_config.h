#ifndef OTA_UPDATE_CONFIG_H_
#define OTA_UPDATE_CONFIG_H_

/*
 * The generated FSP project now contains g_ota_flash and g_iwdt0, and the
 * MCUboot SmartBundle layout has been checked against the generated
 * sysflash.h.
 */
#ifndef OTA_UPDATE_FSP_READY
 #define OTA_UPDATE_FSP_READY                (1)
#endif

#define OTA_UPDATE_DEVICE_ID                 "fridge-main-01"
#define OTA_UPDATE_PART_NUMBER               "R7FA6M5BF2CBG"
#define OTA_UPDATE_APP_VERSION               "1.0.0+22"
#define OTA_UPDATE_BOOT_VERSION              "1.0.0+0"

/*
 * Set this to the same random value as FRIDGE_OTA_TOKEN in
 * /etc/fridge-deepseek.env.  Do not reuse the DashScope API key or signing
 * private key.
 */
#define OTA_UPDATE_DEVICE_TOKEN              "IfReqauKkPH2ZhPJmHmWqT7gaBtIrMl0Ue4g2NL2yZk"

#define OTA_UPDATE_CHECK_INTERVAL_MS         (1000U)
#define OTA_UPDATE_NETWORK_TIMEOUT_MS        (12000U)
#define OTA_UPDATE_CHUNK_BYTES               (4096U)
#define OTA_UPDATE_CHECKPOINT_BYTES          (32U * 1024U)
#define OTA_UPDATE_TRIAL_CONFIRM_MS           (10000U)
#define OTA_UPDATE_MAX_TRIAL_BOOTS            (3U)
#define OTA_UPDATE_INITIAL_ACTIVE_BANK         (0U)

/*
 * Generated RA6M5 Direct-XIP dual-bank layout:
 *
 *   logical primary image body : 0x00020200 .. 0x00080000
 *   physical inactive slot     : 0x00220000 .. 0x00280000
 *
 * The OTA file is the complete padded *.bin.signed image, so it is written
 * at the slot base (including the 0x200-byte MCUboot header), not at the
 * logical image-body address.  In dual-bank mode FSP maps the inactive bank
 * through BSP_FEATURE_FLASH_HP_CF_DUAL_BANK_START (0x00200000).
 */
#ifndef OTA_UPDATE_INACTIVE_SLOT_ADDRESS
 #define OTA_UPDATE_INACTIVE_SLOT_ADDRESS      (0x00220000U)
#endif
#ifndef OTA_UPDATE_INACTIVE_SLOT_SIZE
 #define OTA_UPDATE_INACTIVE_SLOT_SIZE         (0x00060000U)
#endif
#ifndef OTA_UPDATE_INACTIVE_SLOT_ERASE_BLOCKS
 #define OTA_UPDATE_INACTIVE_SLOT_ERASE_BLOCKS (12U)
#endif
#ifndef OTA_UPDATE_CODE_FLASH_ERASE_BYTES
 #define OTA_UPDATE_CODE_FLASH_ERASE_BYTES     (0x00008000U)
#endif

/*
 * RA6M5 has 8 KB Data Flash.  The OTA journal uses only the final 512 bytes:
 * two alternating 256-byte records.  Change these only if the generated
 * linker/Flash layout reserves a different area.
 */
#define OTA_UPDATE_METADATA_SLOT0_ADDRESS     (0x08001E00U)
#define OTA_UPDATE_METADATA_SLOT1_ADDRESS     (0x08001F00U)
#define OTA_UPDATE_METADATA_ERASE_BLOCKS      (4U)

#if OTA_UPDATE_FSP_READY
 #if (0U == OTA_UPDATE_INACTIVE_SLOT_ADDRESS) || \
     (0U == OTA_UPDATE_INACTIVE_SLOT_SIZE) || \
     (0U == OTA_UPDATE_INACTIVE_SLOT_ERASE_BLOCKS) || \
     (0U == OTA_UPDATE_CODE_FLASH_ERASE_BYTES)
  #error "Copy the generated MCUboot inactive-slot layout into ota_update_config.h"
 #endif
 #if (OTA_UPDATE_CHECKPOINT_BYTES % OTA_UPDATE_CODE_FLASH_ERASE_BYTES)
  #error "OTA checkpoint size must align to the generated Code Flash erase size"
 #endif
 #if (OTA_UPDATE_INACTIVE_SLOT_ADDRESS % OTA_UPDATE_CODE_FLASH_ERASE_BYTES)
  #error "Inactive slot address must align to its first Code Flash erase block"
 #endif
 #if (OTA_UPDATE_INACTIVE_SLOT_SIZE % OTA_UPDATE_CODE_FLASH_ERASE_BYTES)
  #error "Inactive slot size must contain whole Code Flash erase blocks"
 #endif
#endif

#endif /* OTA_UPDATE_CONFIG_H_ */
