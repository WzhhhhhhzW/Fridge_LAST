#include "system_tick.h"
#include "DOOR.h"
#include "pn532.h"

volatile uint32_t g_system_ms = 0U;

void system_tick_callback(timer_callback_args_t * p_args)
{
    if ((NULL != p_args) && (TIMER_EVENT_CYCLE_END == p_args->event))
    {
        g_system_ms++;
        door_tick_1ms(g_system_ms);
        pn532_tick_1ms();
    }
}

fsp_err_t system_tick_init(void)
{
    fsp_err_t err = R_GPT_Open(&g_system_tick_ctrl, &g_system_tick_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        return err;
    }

    return R_GPT_Start(&g_system_tick_ctrl);
}

uint32_t system_tick_now(void)
{
    return g_system_ms;
}

bool system_tick_elapsed(uint32_t start_ms, uint32_t delay_ms)
{
    return ((uint32_t)(system_tick_now() - start_ms) >= delay_ms);
}
