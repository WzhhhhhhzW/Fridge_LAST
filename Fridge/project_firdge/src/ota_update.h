#ifndef OTA_UPDATE_H_
#define OTA_UPDATE_H_

#include "hal_data.h"

#include <stdbool.h>

fsp_err_t ota_update_init(void);
void ota_update_process(void);
bool ota_update_active(void);
void ota_update_confirm_current(void);
void ota_update_watchdog_refresh(void);

#endif /* OTA_UPDATE_H_ */
