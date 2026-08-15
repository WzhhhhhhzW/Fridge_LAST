#ifndef OTA_BOOT_TRIAL_H_
#define OTA_BOOT_TRIAL_H_

#include "bootutil/bootutil.h"
#include "hal_data.h"

/*
 * Apply the application-owned trial/rollback journal after MCUboot has
 * authenticated and selected an image, but before RM_MCUBOOT_PORT_BootApp().
 *
 * This function returns only when the selected image may be launched.  A
 * required bank swap or failed-image invalidation is followed by a reset.
 */
fsp_err_t ota_boot_trial_process(struct boot_rsp const * p_boot_response);

#endif /* OTA_BOOT_TRIAL_H_ */
