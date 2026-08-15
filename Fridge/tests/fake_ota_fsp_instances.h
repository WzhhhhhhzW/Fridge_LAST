#ifndef FAKE_OTA_FSP_INSTANCES_H_
#define FAKE_OTA_FSP_INSTANCES_H_

/*
 * Compile-only stand-ins used to syntax-check OTA_UPDATE_FSP_READY=1 before
 * the user generates g_ota_flash and g_iwdt0.  These are never included by
 * the RA6M5 application build.
 */
#include "../project_firdge/ra_gen/hal_data.h"

typedef struct st_fake_flash_ctrl
{
    uint32_t placeholder;
} flash_ctrl_t;

typedef struct st_fake_flash_cfg
{
    uint32_t placeholder;
} flash_cfg_t;

typedef struct st_fake_iwdt_ctrl
{
    uint32_t placeholder;
} iwdt_ctrl_t;

typedef struct st_fake_iwdt_cfg
{
    uint32_t placeholder;
} iwdt_cfg_t;

extern flash_ctrl_t g_ota_flash_ctrl;
extern const flash_cfg_t g_ota_flash_cfg;
extern iwdt_ctrl_t g_iwdt0_ctrl;
extern const iwdt_cfg_t g_iwdt0_cfg;

fsp_err_t R_FLASH_HP_Open(flash_ctrl_t * p_ctrl, const flash_cfg_t * p_cfg);
fsp_err_t R_FLASH_HP_Erase(flash_ctrl_t * p_ctrl, uint32_t address, uint32_t blocks);
fsp_err_t R_FLASH_HP_Write(flash_ctrl_t * p_ctrl,
                           uint32_t source,
                           uint32_t destination,
                           uint32_t bytes);
fsp_err_t R_IWDT_Open(iwdt_ctrl_t * p_ctrl, const iwdt_cfg_t * p_cfg);
fsp_err_t R_IWDT_Refresh(iwdt_ctrl_t * p_ctrl);

#endif /* FAKE_OTA_FSP_INSTANCES_H_ */
