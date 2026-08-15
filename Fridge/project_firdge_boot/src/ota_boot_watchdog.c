#include "hal_data.h"
#include "ota_boot_watchdog.h"

static bool g_ota_boot_iwdt_open = false;

void ota_boot_watchdog_refresh(void)
{
    if (!g_ota_boot_iwdt_open)
    {
        fsp_err_t open_error = R_IWDT_Open(&g_iwdt0_ctrl, &g_iwdt0_cfg);
        if ((FSP_SUCCESS == open_error) || (FSP_ERR_ALREADY_OPEN == open_error))
        {
            g_ota_boot_iwdt_open = true;
        }
    }

    if (g_ota_boot_iwdt_open)
    {
        (void) R_IWDT_Refresh(&g_iwdt0_ctrl);
    }
}
