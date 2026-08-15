#ifndef SYSTEM_TICK_H
#define SYSTEM_TICK_H

#include "hal_data.h"
#include <stdbool.h>
#include <stdint.h>

extern volatile uint32_t g_system_ms;

fsp_err_t system_tick_init(void);
uint32_t system_tick_now(void);
bool system_tick_elapsed(uint32_t start_ms, uint32_t delay_ms);
void system_tick_callback(timer_callback_args_t * p_args);

#endif /* SYSTEM_TICK_H */
