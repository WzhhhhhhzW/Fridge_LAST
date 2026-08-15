#ifndef FAN_CONTROL_H
#define FAN_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_data.h"


#define FAN_FG1_PULSES_PER_REV    (2U)
#define FAN_FG2_PULSES_PER_REV    (2U)

typedef enum e_cooling_interlock_state
{
    COOLING_INTERLOCK_OFF = 0,
    COOLING_INTERLOCK_PREHEAT,
    COOLING_INTERLOCK_RUNNING,
} cooling_interlock_state_t;


void fan_init(void);

// 设置风扇速度
void black_fan_set_speed(uint8_t speed_percent);
fsp_err_t water_cooling_set_enabled(bool enabled);
bool water_cooling_output_is_enabled(void);
// 获取当前风扇速度
uint8_t fan_get_speed(void);

fsp_err_t fan_tach_init(void);
void fan_tach_process(void);
uint32_t fan_tach_get_rpm(uint8_t fan_index);

fsp_err_t cooling_interlock_init(void);
void cooling_interlock_process(void);
void cooling_interlock_submit_code(uint8_t code);
void cooling_interlock_set_targets(uint8_t fan, bool water, bool cooling);
void cooling_interlock_water_output_applied(bool enabled);
void cooling_interlock_abort(void);
cooling_interlock_state_t cooling_interlock_state_get(void);

#endif
