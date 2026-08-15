#ifndef INA226_POWER_H_
#define INA226_POWER_H_

#include "hal_data.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    bool valid;
    uint8_t i2c_address;
    int32_t bus_mv;
    int32_t current_mA;
    int32_t power_dW;
} ina226_power_sample_t;

void ina226_power_init(void);
void ina226_power_process(void);
bool ina226_power_get_latest(uint8_t i2c_address, ina226_power_sample_t * p_sample);

#if defined(R_I2C_MASTER_API_H)
void ina226_i2c_callback(i2c_master_callback_args_t * p_args);
#endif

#endif /* INA226_POWER_H_ */
