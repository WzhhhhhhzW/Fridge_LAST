#ifndef HEALTH_ASSISTANT_H_
#define HEALTH_ASSISTANT_H_

#include "hal_data.h"

#include <stddef.h>
#include <stdint.h>

#define HEALTH_ASSISTANT_NAME_MAX          160U
#define HEALTH_ASSISTANT_DIAGNOSIS_MAX     64U
#define HEALTH_ASSISTANT_SYMPTOM_MAX       512U

typedef enum e_health_result
{
    HEALTH_RESULT_OK = 0,
    HEALTH_RESULT_BUSY,
    HEALTH_RESULT_NETWORK_ERROR,
    HEALTH_RESULT_INVALID_ARGUMENT,
    HEALTH_RESULT_SERVER_ERROR,
    HEALTH_RESULT_PARSE_ERROR,
} health_result_t;

const char * health_assistant_symptom_from_code(uint8_t symptom_code);
health_result_t health_assistant_assess_by_text(const char * symptom,
                                                char * out_diagnosis,
                                                size_t diagnosis_size,
                                                char * out_medicine,
                                                size_t medicine_size);
health_result_t health_assistant_recommend_by_text(const char * symptom,
                                                   char * out_name,
                                                   size_t out_size);
health_result_t health_assistant_recommend_by_code(uint8_t symptom_code,
                                                   char * out_name,
                                                   size_t out_size);
const char * health_result_text(health_result_t result);

#endif /* HEALTH_ASSISTANT_H_ */
