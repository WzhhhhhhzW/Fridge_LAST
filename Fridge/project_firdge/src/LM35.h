#ifndef LM35_H
#define LM35_H


#include "hal_data.h"

#define TEMPERATURE_SENSOR_TMP235 (1U)
#define TEMPERATURE_SENSOR_LM35   (2U)

/* Change this definition to switch the sensor connected to P010/AN010. */
#ifndef TEMPERATURE_SENSOR_ACTIVE
 #define TEMPERATURE_SENSOR_ACTIVE TEMPERATURE_SENSOR_TMP235
#endif

#if ((TEMPERATURE_SENSOR_ACTIVE != TEMPERATURE_SENSOR_TMP235) && \
     (TEMPERATURE_SENSOR_ACTIVE != TEMPERATURE_SENSOR_LM35))
 #error "TEMPERATURE_SENSOR_ACTIVE must be TEMPERATURE_SENSOR_LM35 or TEMPERATURE_SENSOR_TMP235"
#endif


#ifdef __cplusplus
extern "C" //不加就有神秘bug
{
    #endif


    /* Legacy API names retained; implementation supports LM35 and TMP235. */
    fsp_err_t lm35_init(void); //初始化TMP235及ADC

    /* Non-blocking ADC service. Call once from every main-loop pass. */
    void lm35_process(void);

    fsp_err_t lm35_read_temperature(float * temperature_c); //读取TMP235温度值

    /* Read the filtered active-sensor temperature before calibration offset. */
    fsp_err_t lm35_read_raw_temperature(float * temperature_c);
    const char * lm35_sensor_name(void);


    #ifdef __cplusplus
}
#endif


#endif /* LM35_H */
