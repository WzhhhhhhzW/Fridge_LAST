#include "ota_boot_trial.h"

#include <stddef.h>

static uint32_t ota_boot_crc32(const uint8_t * p_data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < length; i++)
    {
        crc ^= p_data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            uint32_t mask = (uint32_t) (-(int32_t) (crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

bool ota_boot_trial_record_valid(const ota_boot_record_t * p_record)
{
    if ((NULL == p_record) ||
        (OTA_BOOT_RECORD_MAGIC != p_record->magic) ||
        (OTA_BOOT_RECORD_SCHEMA != p_record->schema) ||
        (p_record->state > OTA_BOOT_STATE_ROLLBACK) ||
        (p_record->target_bank > 1U))
    {
        return false;
    }

    return p_record->crc32 ==
           ota_boot_crc32((const uint8_t *) p_record,
                          offsetof(ota_boot_record_t, crc32));
}

void ota_boot_trial_record_seal(ota_boot_record_t * p_record)
{
    if (NULL == p_record)
    {
        return;
    }

    p_record->magic = OTA_BOOT_RECORD_MAGIC;
    p_record->schema = OTA_BOOT_RECORD_SCHEMA;
    p_record->generation++;
    p_record->crc32 = ota_boot_crc32((const uint8_t *) p_record,
                                     offsetof(ota_boot_record_t, crc32));
}

ota_boot_trial_action_t ota_boot_trial_before_launch(ota_boot_record_t * p_record,
                                                     uint8_t selected_bank,
                                                     uint32_t max_trial_boots)
{
    if ((!ota_boot_trial_record_valid(p_record)) ||
        (selected_bank > 1U) ||
        (0U == max_trial_boots))
    {
        return OTA_BOOT_TRIAL_ACTION_UNCHANGED;
    }

    if ((OTA_BOOT_STATE_PENDING_TRIAL != p_record->state) ||
        (selected_bank != p_record->target_bank))
    {
        return OTA_BOOT_TRIAL_ACTION_UNCHANGED;
    }

    if (p_record->trial_boots >= max_trial_boots)
    {
        p_record->state = OTA_BOOT_STATE_ROLLBACK;
        ota_boot_trial_record_seal(p_record);
        return OTA_BOOT_TRIAL_ACTION_STORE_AND_ROLL_BACK;
    }

    p_record->trial_boots++;
    ota_boot_trial_record_seal(p_record);
    return OTA_BOOT_TRIAL_ACTION_STORE_AND_BOOT;
}
