#include "health_assistant.h"
#include "deepseek_bridge.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define HEALTH_REQUEST_TIMEOUT_MS       60000U
#define HEALTH_COMMAND_BUFFER_SIZE      640U
#define HEALTH_RESPONSE_BUFFER_SIZE     256U

static bool starts_with(const char * text, const char * prefix)
{
    return (0 == strncmp(text, prefix, strlen(prefix)));
}

static void copy_text(char * dst, size_t dst_size, const char * src)
{
    if ((NULL == dst) || (0U == dst_size))
    {
        return;
    }

    if (NULL == src)
    {
        dst[0] = '\0';
        return;
    }

    (void) snprintf(dst, dst_size, "%s", src);
}

static bool symptom_is_valid(const char * symptom)
{
    size_t length;

    if (NULL == symptom)
    {
        return false;
    }

    length = strlen(symptom);
    if ((0U == length) || (length >= HEALTH_ASSISTANT_SYMPTOM_MAX))
    {
        return false;
    }

    for (size_t i = 0U; i < length; i++)
    {
        if ((symptom[i] == '|') || (symptom[i] == '\r') || (symptom[i] == '\n'))
        {
            return false;
        }
    }

    return true;
}

const char * health_assistant_symptom_from_code(uint8_t symptom_code)
{
    switch (symptom_code)
    {
        case 0x0BU:
            return "头疼";

        case 0x0CU:
            return "发烧";

        case 0x0DU:
            return "呕吐";

        case 0x0EU:
            return "喉咙疼";

        case 0x0FU:
            return "流鼻涕";

        case 0xA1U:
            return "咳嗽";

        case 0xA2U:
            return "打喷嚏";

        default:
            return NULL;
    }
}

static health_result_t map_bridge_error(fsp_err_t err)
{
    if (FSP_SUCCESS == err)
    {
        return HEALTH_RESULT_OK;
    }

    if (FSP_ERR_IN_USE == err)
    {
        return HEALTH_RESULT_BUSY;
    }

    if (FSP_ERR_INVALID_ARGUMENT == err)
    {
        return HEALTH_RESULT_INVALID_ARGUMENT;
    }

    return HEALTH_RESULT_NETWORK_ERROR;
}

health_result_t health_assistant_assess_by_text(const char * symptom,
                                                char * out_diagnosis,
                                                size_t diagnosis_size,
                                                char * out_medicine,
                                                size_t medicine_size)
{
    static const char ok_prefix[] = "[HA] OK ";
    char request[HEALTH_COMMAND_BUFFER_SIZE];
    char response[HEALTH_RESPONSE_BUFFER_SIZE];
    char * separator;
    char * payload;
    fsp_err_t err;
    int written;

    if ((NULL == out_diagnosis) || (0U == diagnosis_size) ||
        (NULL == out_medicine) || (0U == medicine_size) ||
        (!symptom_is_valid(symptom)))
    {
        return HEALTH_RESULT_INVALID_ARGUMENT;
    }

    out_diagnosis[0] = '\0';
    out_medicine[0] = '\0';
    written = snprintf(request, sizeof(request), "HA ASSESS %s", symptom);
    if ((written < 0) || ((size_t) written >= sizeof(request)))
    {
        return HEALTH_RESULT_INVALID_ARGUMENT;
    }

    err = deepseek_bridge_server_request(request,
                                         response,
                                         sizeof(response),
                                         HEALTH_REQUEST_TIMEOUT_MS);
    if (FSP_SUCCESS != err)
    {
        return map_bridge_error(err);
    }

    if (starts_with(response, ok_prefix))
    {
        payload = response + strlen(ok_prefix);
        separator = strchr(payload, '|');
        if (NULL == separator)
        {
            return HEALTH_RESULT_PARSE_ERROR;
        }

        *separator = '\0';
        if (('\0' == payload[0]) || ('\0' == separator[1]))
        {
            return HEALTH_RESULT_PARSE_ERROR;
        }

        copy_text(out_diagnosis, diagnosis_size, payload);
        copy_text(out_medicine, medicine_size, separator + 1);
        return HEALTH_RESULT_OK;
    }

    if (starts_with(response, "[HA] ERR"))
    {
        return HEALTH_RESULT_SERVER_ERROR;
    }

    return HEALTH_RESULT_PARSE_ERROR;
}

health_result_t health_assistant_recommend_by_text(const char * symptom,
                                                   char * out_name,
                                                   size_t out_size)
{
    static const char ok_prefix[] = "[MED] OK RECOMMEND ";
    char request[HEALTH_COMMAND_BUFFER_SIZE];
    char response[HEALTH_RESPONSE_BUFFER_SIZE];
    fsp_err_t err;
    int written;

    if ((NULL == out_name) || (0U == out_size) || (!symptom_is_valid(symptom)))
    {
        return HEALTH_RESULT_INVALID_ARGUMENT;
    }

    out_name[0] = '\0';
    written = snprintf(request, sizeof(request), "MED RECOMMEND %s", symptom);
    if ((written < 0) || ((size_t) written >= sizeof(request)))
    {
        return HEALTH_RESULT_INVALID_ARGUMENT;
    }

    err = deepseek_bridge_server_request(request,
                                         response,
                                         sizeof(response),
                                         HEALTH_REQUEST_TIMEOUT_MS);
    if (FSP_SUCCESS != err)
    {
        return map_bridge_error(err);
    }

    if (starts_with(response, ok_prefix))
    {
        copy_text(out_name, out_size, response + strlen(ok_prefix));
        return HEALTH_RESULT_OK;
    }

    if (starts_with(response, "[MED] ERR"))
    {
        return HEALTH_RESULT_SERVER_ERROR;
    }

    return HEALTH_RESULT_PARSE_ERROR;
}

health_result_t health_assistant_recommend_by_code(uint8_t symptom_code,
                                                   char * out_name,
                                                   size_t out_size)
{
    const char * symptom = health_assistant_symptom_from_code(symptom_code);
    if (NULL == symptom)
    {
        return HEALTH_RESULT_INVALID_ARGUMENT;
    }

    return health_assistant_recommend_by_text(symptom, out_name, out_size);
}

const char * health_result_text(health_result_t result)
{
    switch (result)
    {
        case HEALTH_RESULT_OK:
            return "OK";

        case HEALTH_RESULT_BUSY:
            return "BUSY";

        case HEALTH_RESULT_NETWORK_ERROR:
            return "NETWORK_ERROR";

        case HEALTH_RESULT_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";

        case HEALTH_RESULT_SERVER_ERROR:
            return "SERVER_ERROR";

        case HEALTH_RESULT_PARSE_ERROR:
        default:
            return "PARSE_ERROR";
    }
}
