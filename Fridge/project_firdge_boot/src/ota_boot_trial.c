#include "ota_boot_trial.h"

#include "../../project_firdge/src/ota_boot_record.h"
#include "bootutil/image.h"
#include "ota_boot_watchdog.h"

#include <stddef.h>
#include <string.h>

#define OTA_BOOT_METADATA_SLOT0_ADDRESS       (0x08001E00U)
#define OTA_BOOT_METADATA_SLOT1_ADDRESS       (0x08001F00U)
#define OTA_BOOT_METADATA_ERASE_BLOCKS        (4U)
#define OTA_BOOT_INACTIVE_SLOT_ADDRESS        (0x00220000U)
#define OTA_BOOT_FAILED_HEADER_ERASE_BLOCKS   (1U)
#define OTA_BOOT_MAX_TRIAL_FAILURES           (3U)

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

static bool ota_boot_record_valid(ota_boot_record_t const * p_record)
{
    uint32_t expected_crc;

    if ((NULL == p_record) ||
        (OTA_BOOT_RECORD_MAGIC != p_record->magic) ||
        (OTA_BOOT_RECORD_SCHEMA != p_record->schema) ||
        (p_record->state > OTA_BOOT_STATE_ROLLBACK) ||
        (p_record->target_bank > 1U))
    {
        return false;
    }

    expected_crc = ota_boot_crc32((uint8_t const *) p_record,
                                  offsetof(ota_boot_record_t, crc32));
    return expected_crc == p_record->crc32;
}

static bool ota_boot_record_load(ota_boot_record_t * p_record,
                                 uint32_t * p_source_slot)
{
    ota_boot_record_t const * p_slot0 =
        (ota_boot_record_t const *) OTA_BOOT_METADATA_SLOT0_ADDRESS;
    ota_boot_record_t const * p_slot1 =
        (ota_boot_record_t const *) OTA_BOOT_METADATA_SLOT1_ADDRESS;
    bool slot0_valid = ota_boot_record_valid(p_slot0);
    bool slot1_valid = ota_boot_record_valid(p_slot1);

    if ((NULL == p_record) || (NULL == p_source_slot))
    {
        return false;
    }

    if (slot0_valid && slot1_valid)
    {
        if ((int32_t) (p_slot1->generation - p_slot0->generation) > 0)
        {
            memcpy(p_record, p_slot1, sizeof(*p_record));
            *p_source_slot = OTA_BOOT_METADATA_SLOT1_ADDRESS;
        }
        else
        {
            memcpy(p_record, p_slot0, sizeof(*p_record));
            *p_source_slot = OTA_BOOT_METADATA_SLOT0_ADDRESS;
        }
        return true;
    }

    if (slot0_valid)
    {
        memcpy(p_record, p_slot0, sizeof(*p_record));
        *p_source_slot = OTA_BOOT_METADATA_SLOT0_ADDRESS;
        return true;
    }

    if (slot1_valid)
    {
        memcpy(p_record, p_slot1, sizeof(*p_record));
        *p_source_slot = OTA_BOOT_METADATA_SLOT1_ADDRESS;
        return true;
    }

    return false;
}

static fsp_err_t ota_boot_record_save(ota_boot_record_t * p_record,
                                      uint32_t source_slot)
{
    uint32_t destination_slot =
        (OTA_BOOT_METADATA_SLOT0_ADDRESS == source_slot) ?
        OTA_BOOT_METADATA_SLOT1_ADDRESS :
        OTA_BOOT_METADATA_SLOT0_ADDRESS;
    fsp_err_t flash_error;

    p_record->magic = OTA_BOOT_RECORD_MAGIC;
    p_record->schema = OTA_BOOT_RECORD_SCHEMA;
    p_record->generation++;
    p_record->crc32 = ota_boot_crc32((uint8_t const *) p_record,
                                     offsetof(ota_boot_record_t, crc32));

    ota_boot_watchdog_refresh();
    flash_error = R_FLASH_HP_Erase(&g_flash0_ctrl,
                                   destination_slot,
                                   OTA_BOOT_METADATA_ERASE_BLOCKS);
    if (FSP_SUCCESS != flash_error)
    {
        return flash_error;
    }

    ota_boot_watchdog_refresh();
    return R_FLASH_HP_Write(&g_flash0_ctrl,
                            (uint32_t) p_record,
                            destination_slot,
                            sizeof(*p_record));
}

static bool ota_boot_parse_u32(char const ** pp_text,
                               char delimiter,
                               uint32_t maximum,
                               uint32_t * p_value)
{
    char const * p_text = *pp_text;
    uint32_t value = 0U;
    bool found_digit = false;

    while ((*p_text >= '0') && (*p_text <= '9'))
    {
        uint32_t digit = (uint32_t) (*p_text - '0');
        if (value > ((maximum - digit) / 10U))
        {
            return false;
        }
        value = (value * 10U) + digit;
        found_digit = true;
        p_text++;
    }

    if ((!found_digit) || (*p_text != delimiter))
    {
        return false;
    }

    *p_value = value;
    *pp_text = p_text + 1;
    return true;
}

static bool ota_boot_selected_version_matches(struct boot_rsp const * p_response,
                                              char const * p_target_version)
{
    char const * p_text = p_target_version;
    uint32_t major;
    uint32_t minor;
    uint32_t revision;
    uint32_t build;
    struct image_version const * p_version;

    if ((NULL == p_response) ||
        (NULL == p_response->br_hdr) ||
        (NULL == p_target_version))
    {
        return false;
    }

    if ((!ota_boot_parse_u32(&p_text, '.', UINT8_MAX, &major)) ||
        (!ota_boot_parse_u32(&p_text, '.', UINT8_MAX, &minor)) ||
        (!ota_boot_parse_u32(&p_text, '+', UINT16_MAX, &revision)) ||
        (!ota_boot_parse_u32(&p_text, '\0', UINT32_MAX, &build)))
    {
        return false;
    }

    p_version = &p_response->br_hdr->ih_ver;
    return (p_version->iv_major == (uint8_t) major) &&
           (p_version->iv_minor == (uint8_t) minor) &&
           (p_version->iv_revision == (uint16_t) revision) &&
           (p_version->iv_build_num == build);
}

static bool ota_boot_selected_from_inactive_alias(struct boot_rsp const * p_response)
{
    return p_response->br_image_off >= BSP_FEATURE_FLASH_HP_CF_DUAL_BANK_START;
}

static fsp_err_t ota_boot_invalidate_inactive_image(void)
{
    ota_boot_watchdog_refresh();
    return R_FLASH_HP_Erase(&g_flash0_ctrl,
                            OTA_BOOT_INACTIVE_SLOT_ADDRESS,
                            OTA_BOOT_FAILED_HEADER_ERASE_BLOCKS);
}

static void ota_boot_reset(void)
{
    ota_boot_watchdog_refresh();
    __DSB();
    NVIC_SystemReset();
    while (1)
    {
        __WFI();
    }
}

static fsp_err_t ota_boot_force_rollback(struct boot_rsp const * p_response)
{
    fsp_err_t flash_error;

    if (ota_boot_selected_from_inactive_alias(p_response))
    {
        /*
         * The failed image is in the currently inactive physical bank.  Its
         * first 32-KB block contains the MCUboot header, so erasing that block
         * prevents it from being selected again.
         */
        flash_error = ota_boot_invalidate_inactive_image();
        if (FSP_SUCCESS != flash_error)
        {
            return flash_error;
        }
        ota_boot_reset();
    }

    /*
     * The failed image is executing from the active bank.  Swap back first;
     * after reset the failed bank appears at the inactive alias and is erased
     * by the rollback-state path.
     */
    ota_boot_watchdog_refresh();
    flash_error = R_FLASH_HP_BankSwap(&g_flash0_ctrl);
    if (FSP_SUCCESS != flash_error)
    {
        return flash_error;
    }
    ota_boot_reset();
    return FSP_SUCCESS;
}

fsp_err_t ota_boot_trial_process(struct boot_rsp const * p_boot_response)
{
    ota_boot_record_t record;
    uint32_t source_slot = OTA_BOOT_METADATA_SLOT0_ADDRESS;
    bool selected_target;
    bool previous_iwdt_reset;
    fsp_err_t flash_error;

    if (!ota_boot_record_load(&record, &source_slot))
    {
        return FSP_SUCCESS;
    }

    selected_target =
        ota_boot_selected_version_matches(p_boot_response, record.target_version);
    previous_iwdt_reset = (0U != R_SYSTEM->RSTSR1_b.IWDTRF);
    if (previous_iwdt_reset)
    {
        R_SYSTEM->RSTSR1_b.IWDTRF = 0U;
    }

    if (OTA_BOOT_STATE_PENDING_TRIAL == record.state)
    {
        if (!selected_target)
        {
            record.state = OTA_BOOT_STATE_ROLLBACK;
            flash_error = ota_boot_record_save(&record, source_slot);
            if (FSP_SUCCESS != flash_error)
            {
                return flash_error;
            }

            /*
             * MCUboot rejected the candidate and selected the old image.
             * If the old image is already active, remove the rejected header
             * from the inactive alias.  If the old image is at the inactive
             * alias, RM_MCUBOOT_PORT_BootApp() will perform the bank swap.
             */
            if (!ota_boot_selected_from_inactive_alias(p_boot_response))
            {
                return ota_boot_invalidate_inactive_image();
            }
            return FSP_SUCCESS;
        }

        if (previous_iwdt_reset)
        {
            record.trial_boots++;
            if (record.trial_boots >= OTA_BOOT_MAX_TRIAL_FAILURES)
            {
                record.state = OTA_BOOT_STATE_ROLLBACK;
            }

            flash_error = ota_boot_record_save(&record, source_slot);
            if (FSP_SUCCESS != flash_error)
            {
                return flash_error;
            }
        }

        if (OTA_BOOT_STATE_ROLLBACK == record.state)
        {
            return ota_boot_force_rollback(p_boot_response);
        }
        return FSP_SUCCESS;
    }

    if ((OTA_BOOT_STATE_ROLLBACK == record.state) && selected_target)
    {
        return ota_boot_force_rollback(p_boot_response);
    }

    return FSP_SUCCESS;
}
