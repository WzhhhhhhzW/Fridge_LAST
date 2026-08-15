#ifndef OTA_BOOT_TRIAL_H_
#define OTA_BOOT_TRIAL_H_

#include "../project_firdge/src/ota_boot_record.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum e_ota_boot_trial_action
{
    OTA_BOOT_TRIAL_ACTION_UNCHANGED = 0,
    OTA_BOOT_TRIAL_ACTION_STORE_AND_BOOT,
    OTA_BOOT_TRIAL_ACTION_STORE_AND_ROLL_BACK,
} ota_boot_trial_action_t;

bool ota_boot_trial_record_valid(const ota_boot_record_t * p_record);
void ota_boot_trial_record_seal(ota_boot_record_t * p_record);

/*
 * Call after MCUboot/SCE9 has authenticated the selected image and before
 * chain-loading it.  Persist p_record whenever the return value requests it.
 * For STORE_AND_ROLL_BACK, invalidate the failed image and select the other
 * bank instead of launching selected_bank.
 */
ota_boot_trial_action_t ota_boot_trial_before_launch(ota_boot_record_t * p_record,
                                                     uint8_t selected_bank,
                                                     uint32_t max_trial_boots);

#endif /* OTA_BOOT_TRIAL_H_ */
