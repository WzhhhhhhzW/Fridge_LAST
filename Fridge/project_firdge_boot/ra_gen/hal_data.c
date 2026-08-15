/* generated HAL source file - do not edit */
#include "hal_data.h"
iwdt_instance_ctrl_t g_iwdt0_ctrl;

const wdt_cfg_t g_iwdt0_cfg =
{ .timeout = (wdt_timeout_t) 0,
  .clock_division = (wdt_clock_division_t) 0,
  .window_start = (wdt_window_start_t) 0,
  .window_end = (wdt_window_end_t) 0,
  .reset_control = (wdt_reset_control_t) 0,
  .stop_control = (wdt_stop_control_t) 0,
  .p_callback = NULL, };

/* Instance structure to use this module. */
const wdt_instance_t g_iwdt0 =
{ .p_ctrl = &g_iwdt0_ctrl, .p_cfg = &g_iwdt0_cfg, .p_api = &g_wdt_on_iwdt };
flash_hp_instance_ctrl_t g_flash0_ctrl;
const flash_cfg_t g_flash0_cfg =
{ .data_flash_bgo = false, .p_callback = NULL, .p_context = NULL,
#if defined(VECTOR_NUMBER_FCU_FRDYI)
    .irq                 = VECTOR_NUMBER_FCU_FRDYI,
#else
  .irq = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_FCU_FIFERR)
    .err_irq             = VECTOR_NUMBER_FCU_FIFERR,
#else
  .err_irq = FSP_INVALID_VECTOR,
#endif
  .err_ipl = (BSP_IRQ_DISABLED),
  .ipl = (BSP_IRQ_DISABLED), };
/* Instance structure to use this module. */
const flash_instance_t g_flash0 =
{ .p_ctrl = &g_flash0_ctrl, .p_cfg = &g_flash0_cfg, .p_api = &g_flash_on_flash_hp };
void *const gp_mcuboot_flash_ctrl = &g_flash0_ctrl;
flash_cfg_t const *const gp_mcuboot_flash_cfg = &g_flash0_cfg;
flash_instance_t const *const gp_mcuboot_flash_instance = &g_flash0;
void g_hal_init(void)
{
    g_common_init ();
}
