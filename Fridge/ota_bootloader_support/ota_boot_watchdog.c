#include "hal_data.h"

#include "ota_boot_watchdog.h"

void ota_boot_watchdog_refresh(void)
{
    (void) R_IWDT_Refresh(&g_iwdt0_ctrl);
}
