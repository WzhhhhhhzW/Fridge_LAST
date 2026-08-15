#include "medicine_storage.h"
#include "deepseek_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEDICINE_REQUEST_TIMEOUT_MS      12000U
#define MEDICINE_REQUEST_BUFFER_SIZE     320U
#define MEDICINE_RESPONSE_BUFFER_SIZE    512U

static char g_medicine_inventory_response[MEDICINE_INVENTORY_TEXT_MAX];

typedef struct st_inventory_display_record
{
    char * name;
    char * production_date;
    char * shelf_life;
    uint16_t quantity;
    size_t name_width;
    size_t quantity_width;
    size_t production_date_width;
} inventory_display_record_t;

static size_t utf8_display_width(const char * text)
{
    size_t index = 0U;
    size_t width = 0U;

    while ((NULL != text) && ('\0' != text[index]))
    {
        uint8_t byte = (uint8_t) text[index];
        size_t sequence_length = 1U;

        if (byte < 0x80U)
        {
            width++;
        }
        else
        {
            /* TJC UTF-8 fonts render Chinese characters as two ASCII cells. */
            width += 2U;
            if ((byte & 0xE0U) == 0xC0U)
            {
                sequence_length = 2U;
            }
            else if ((byte & 0xF0U) == 0xE0U)
            {
                sequence_length = 3U;
            }
            else if ((byte & 0xF8U) == 0xF0U)
            {
                sequence_length = 4U;
            }
        }

        index++;
        for (size_t continuation = 1U;
             (continuation < sequence_length) && ('\0' != text[index]);
             continuation++)
        {
            if ((((uint8_t) text[index]) & 0xC0U) != 0x80U)
            {
                break;
            }
            index++;
        }
    }

    return width;
}

static bool inventory_append(char * output,
                             size_t output_size,
                             size_t * used,
                             const char * text,
                             size_t text_length)
{
    if ((NULL == output) || (NULL == used) || (NULL == text) ||
        ((*used + text_length) >= output_size))
    {
        return false;
    }

    memcpy(&output[*used], text, text_length);
    *used += text_length;
    output[*used] = '\0';
    return true;
}

static bool inventory_append_spaces(char * output,
                                    size_t output_size,
                                    size_t * used,
                                    size_t count)
{
    if ((NULL == output) || (NULL == used) || ((*used + count) >= output_size))
    {
        return false;
    }

    memset(&output[*used], ' ', count);
    *used += count;
    output[*used] = '\0';
    return true;
}

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

static bool field_is_valid(const char * text, size_t max_size)
{
    size_t length;

    if (NULL == text)
    {
        return false;
    }

    length = strlen(text);
    if ((0U == length) || (length >= max_size))
    {
        return false;
    }

    for (size_t i = 0U; i < length; i++)
    {
        if ((text[i] == '|') || (text[i] == '\r') || (text[i] == '\n'))
        {
            return false;
        }
    }

    return true;
}

static bool slot_is_valid(uint8_t slot)
{
    return (slot < MEDICINE_MAX_RECORDS);
}

static bool parse_slot_value(const char * text, uint8_t * out_slot)
{
    char * end = NULL;
    unsigned long value;

    if ((NULL == text) || ('\0' == text[0]) || (NULL == out_slot))
    {
        return false;
    }

    value = strtoul(text, &end, 10);
    if ((end == text) || ('\0' != *end) || (value >= MEDICINE_MAX_RECORDS))
    {
        return false;
    }

    *out_slot = (uint8_t) value;
    return true;
}

static bool parse_quantity_value(const char * text, bool allow_zero, uint16_t * out_quantity)
{
    char * end = NULL;
    unsigned long value;

    if ((NULL == text) || ('\0' == text[0]) || (NULL == out_quantity))
    {
        return false;
    }

    value = strtoul(text, &end, 10);
    if ((end == text) || ('\0' != *end) || (value > MEDICINE_QUANTITY_MAX) ||
        ((!allow_zero) && (0UL == value)))
    {
        return false;
    }

    *out_quantity = (uint16_t) value;
    return true;
}

static uint32_t split_fields(char * text, char ** fields, uint32_t max_fields)
{
    uint32_t count = 0U;

    if ((NULL == text) || (NULL == fields) || (0U == max_fields))
    {
        return 0U;
    }

    fields[count++] = text;
    for (char * p = text; '\0' != *p; p++)
    {
        if ('|' == *p)
        {
            *p = '\0';
            if (count >= max_fields)
            {
                return count + 1U;
            }

            fields[count++] = p + 1;
        }
    }

    return count;
}

static medicine_result_t send_medicine_request(const char * request,
                                               char * response,
                                               size_t response_size)
{
    fsp_err_t err = deepseek_bridge_server_request(request,
                                                   response,
                                                   response_size,
                                                   MEDICINE_REQUEST_TIMEOUT_MS);

    if (FSP_SUCCESS == err)
    {
        return MEDICINE_RESULT_OK;
    }

    if (FSP_ERR_IN_USE == err)
    {
        return MEDICINE_RESULT_BUSY;
    }

    if (FSP_ERR_INVALID_ARGUMENT == err)
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    return MEDICINE_RESULT_NETWORK_ERROR;
}

static medicine_result_t parse_server_error(const char * response)
{
    if (starts_with(response, "[MED] ERR EMPTY"))
    {
        return MEDICINE_RESULT_NOT_FOUND;
    }

    if (starts_with(response, "[MED] ERR INSUFFICIENT"))
    {
        return MEDICINE_RESULT_INSUFFICIENT;
    }

    if (starts_with(response, "[MED] ERR RANGE") ||
        starts_with(response, "[MED] ERR quantity must be"))
    {
        return MEDICINE_RESULT_RANGE_ERROR;
    }

    if (starts_with(response, "[MED] ERR"))
    {
        return MEDICINE_RESULT_SERVER_ERROR;
    }

    return MEDICINE_RESULT_PARSE_ERROR;
}

static medicine_result_t parse_simple_ok(const char * response, const char * ok_prefix)
{
    if (starts_with(response, ok_prefix))
    {
        return MEDICINE_RESULT_OK;
    }

    return parse_server_error(response);
}

static medicine_result_t parse_get_response(char * response, medicine_record_t * out_record)
{
    static const char prefix[] = "[MED] OK GETQ ";
    char * fields[7];
    uint8_t slot = 0U;
    uint16_t quantity = 0U;

    if (!starts_with(response, prefix))
    {
        return parse_server_error(response);
    }

    response += strlen(prefix);
    if (7U != split_fields(response, fields, 7U))
    {
        return MEDICINE_RESULT_PARSE_ERROR;
    }

    if ((!parse_slot_value(fields[0], &slot)) || (!slot_is_valid(slot)))
    {
        return MEDICINE_RESULT_PARSE_ERROR;
    }

    if (0 != strcmp(fields[1], "1"))
    {
        return MEDICINE_RESULT_NOT_FOUND;
    }

    if (!parse_quantity_value(fields[2], false, &quantity))
    {
        return MEDICINE_RESULT_PARSE_ERROR;
    }

    memset(out_record, 0, sizeof(*out_record));
    out_record->valid = true;
    out_record->slot = slot;
    out_record->quantity = quantity;
    copy_text(out_record->name, sizeof(out_record->name), fields[3]);
    copy_text(out_record->production_date, sizeof(out_record->production_date), fields[4]);
    copy_text(out_record->shelf_life, sizeof(out_record->shelf_life), fields[5]);
    copy_text(out_record->efficacy, sizeof(out_record->efficacy), fields[6]);
    return MEDICINE_RESULT_OK;
}

static medicine_result_t parse_quantity_response(char * response,
                                                 const char * operation,
                                                 uint8_t expected_slot,
                                                 uint16_t * out_quantity)
{
    char prefix[32];
    char * fields[2];
    uint8_t slot = 0U;
    uint16_t quantity = 0U;
    int written = snprintf(prefix, sizeof(prefix), "[MED] OK %s ", operation);

    if ((written < 0) || ((size_t) written >= sizeof(prefix)) ||
        (!starts_with(response, prefix)))
    {
        return parse_server_error(response);
    }

    response += strlen(prefix);
    if (2U != split_fields(response, fields, 2U))
    {
        return MEDICINE_RESULT_PARSE_ERROR;
    }

    if ((!parse_slot_value(fields[0], &slot)) || (slot != expected_slot) ||
        (!parse_quantity_value(fields[1], true, &quantity)))
    {
        return MEDICINE_RESULT_PARSE_ERROR;
    }

    if (NULL != out_quantity)
    {
        *out_quantity = quantity;
    }
    return MEDICINE_RESULT_OK;
}

static medicine_result_t medicine_quantity_request(const char * operation,
                                                   uint8_t slot,
                                                   uint16_t value,
                                                   bool allow_zero,
                                                   uint16_t * out_quantity)
{
    char request[48];
    char response[MEDICINE_RESPONSE_BUFFER_SIZE];
    medicine_result_t result;
    int written;

    if ((!slot_is_valid(slot)) || (value > MEDICINE_QUANTITY_MAX) ||
        ((!allow_zero) && (0U == value)))
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    written = snprintf(request,
                       sizeof(request),
                       "MED %s %u|%u",
                       operation,
                       (unsigned int) slot,
                       (unsigned int) value);
    if ((written < 0) || ((size_t) written >= sizeof(request)))
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    result = send_medicine_request(request, response, sizeof(response));
    if (MEDICINE_RESULT_OK != result)
    {
        return result;
    }

    return parse_quantity_response(response, operation, slot, out_quantity);
}

medicine_result_t medicine_add(const char * name,
                               const char * production_date,
                               const char * shelf_life,
                               const char * efficacy,
                               uint8_t * out_slot)
{
    return medicine_add_with_quantity(name,
                                      production_date,
                                      shelf_life,
                                      efficacy,
                                      1U,
                                      out_slot);
}

medicine_result_t medicine_add_with_quantity(const char * name,
                                             const char * production_date,
                                             const char * shelf_life,
                                             const char * efficacy,
                                             uint16_t quantity,
                                             uint8_t * out_slot)
{
    char request[MEDICINE_REQUEST_BUFFER_SIZE];
    char response[MEDICINE_RESPONSE_BUFFER_SIZE];
    medicine_result_t result;
    int written;
    static const char prefix[] = "[MED] OK ADD ";
    uint8_t slot = 0U;

    if ((!field_is_valid(name, MEDICINE_NAME_MAX)) ||
        (!field_is_valid(production_date, MEDICINE_DATE_MAX)) ||
        (!field_is_valid(shelf_life, MEDICINE_SHELF_LIFE_MAX)) ||
        (!field_is_valid(efficacy, MEDICINE_EFFICACY_MAX)) ||
        (0U == quantity) || (quantity > MEDICINE_QUANTITY_MAX))
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    written = snprintf(request,
                       sizeof(request),
                       "MED ADD %s|%s|%s|%s|%u",
                       name,
                       production_date,
                       shelf_life,
                       efficacy,
                       (unsigned int) quantity);
    if ((written < 0) || ((size_t) written >= sizeof(request)))
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    result = send_medicine_request(request, response, sizeof(response));
    if (MEDICINE_RESULT_OK != result)
    {
        return result;
    }

    if (!starts_with(response, prefix))
    {
        return parse_server_error(response);
    }

    if (!parse_slot_value(response + strlen(prefix), &slot))
    {
        return MEDICINE_RESULT_PARSE_ERROR;
    }

    if (NULL != out_slot)
    {
        *out_slot = slot;
    }

    return MEDICINE_RESULT_OK;
}

medicine_result_t medicine_set(uint8_t slot,
                               const char * name,
                               const char * production_date,
                               const char * shelf_life,
                               const char * efficacy)
{
    return medicine_set_with_quantity(slot,
                                      name,
                                      production_date,
                                      shelf_life,
                                      efficacy,
                                      1U);
}

medicine_result_t medicine_set_with_quantity(uint8_t slot,
                                             const char * name,
                                             const char * production_date,
                                             const char * shelf_life,
                                             const char * efficacy,
                                             uint16_t quantity)
{
    char request[MEDICINE_REQUEST_BUFFER_SIZE];
    char response[MEDICINE_RESPONSE_BUFFER_SIZE];
    medicine_result_t result;
    int written;

    if ((!slot_is_valid(slot)) ||
        (!field_is_valid(name, MEDICINE_NAME_MAX)) ||
        (!field_is_valid(production_date, MEDICINE_DATE_MAX)) ||
        (!field_is_valid(shelf_life, MEDICINE_SHELF_LIFE_MAX)) ||
        (!field_is_valid(efficacy, MEDICINE_EFFICACY_MAX)) ||
        (0U == quantity) || (quantity > MEDICINE_QUANTITY_MAX))
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    written = snprintf(request,
                       sizeof(request),
                       "MED SET %u|%s|%s|%s|%s|%u",
                       (unsigned int) slot,
                       name,
                       production_date,
                       shelf_life,
                       efficacy,
                       (unsigned int) quantity);
    if ((written < 0) || ((size_t) written >= sizeof(request)))
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    result = send_medicine_request(request, response, sizeof(response));
    if (MEDICINE_RESULT_OK != result)
    {
        return result;
    }

    return parse_simple_ok(response, "[MED] OK SET ");
}

medicine_result_t medicine_get(uint8_t slot, medicine_record_t * out_record)
{
    char request[32];
    char response[MEDICINE_RESPONSE_BUFFER_SIZE];
    medicine_result_t result;
    int written;

    if ((!slot_is_valid(slot)) || (NULL == out_record))
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    written = snprintf(request, sizeof(request), "MED GETQ %u", (unsigned int) slot);
    if ((written < 0) || ((size_t) written >= sizeof(request)))
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    result = send_medicine_request(request, response, sizeof(response));
    if (MEDICINE_RESULT_OK != result)
    {
        return result;
    }

    return parse_get_response(response, out_record);
}

medicine_result_t medicine_quantity_set(uint8_t slot, uint16_t quantity, uint16_t * out_quantity)
{
    return medicine_quantity_request("QTY", slot, quantity, true, out_quantity);
}

medicine_result_t medicine_restock(uint8_t slot, uint16_t count, uint16_t * out_quantity)
{
    return medicine_quantity_request("RESTOCK", slot, count, false, out_quantity);
}

medicine_result_t medicine_take(uint8_t slot, uint16_t count, uint16_t * out_quantity)
{
    return medicine_quantity_request("TAKE", slot, count, false, out_quantity);
}

medicine_result_t medicine_delete(uint8_t slot)
{
    char request[32];
    char response[MEDICINE_RESPONSE_BUFFER_SIZE];
    medicine_result_t result;
    int written;

    if (!slot_is_valid(slot))
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    written = snprintf(request, sizeof(request), "MED DEL %u", (unsigned int) slot);
    if ((written < 0) || ((size_t) written >= sizeof(request)))
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    result = send_medicine_request(request, response, sizeof(response));
    if (MEDICINE_RESULT_OK != result)
    {
        return result;
    }

    return parse_simple_ok(response, "[MED] OK DEL ");
}

medicine_result_t medicine_list(char * out_text, size_t out_size)
{
    char response[MEDICINE_RESPONSE_BUFFER_SIZE];
    medicine_result_t result;
    static const char prefix[] = "[MED] OK LIST ";

    if ((NULL == out_text) || (0U == out_size))
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    out_text[0] = '\0';
    result = send_medicine_request("MED LIST", response, sizeof(response));
    if (MEDICINE_RESULT_OK != result)
    {
        return result;
    }

    if (!starts_with(response, prefix))
    {
        return parse_server_error(response);
    }

    copy_text(out_text, out_size, response + strlen(prefix));
    return MEDICINE_RESULT_OK;
}

medicine_result_t medicine_inventory_list(char * out_text, size_t out_size)
{
    static const char prefix[] = "[MED] OK LISTFULL ";
    inventory_display_record_t records[MEDICINE_MAX_RECORDS];
    char * payload;
    char * record_text;
    char * next_record;
    char * fields[4];
    size_t used = 0U;
    size_t max_name_width = 0U;
    size_t max_quantity_width = 0U;
    size_t max_production_date_width = 0U;
    uint8_t record_count = 0U;
    medicine_result_t result;

    if ((NULL == out_text) || (0U == out_size))
    {
        return MEDICINE_RESULT_INVALID_ARGUMENT;
    }

    out_text[0] = '\0';
    result = send_medicine_request("MED LISTFULL",
                                   g_medicine_inventory_response,
                                   sizeof(g_medicine_inventory_response));
    if (MEDICINE_RESULT_OK != result)
    {
        return result;
    }

    if (!starts_with(g_medicine_inventory_response, prefix))
    {
        return parse_server_error(g_medicine_inventory_response);
    }

    payload = g_medicine_inventory_response + strlen(prefix);
    if (0 == strcmp(payload, "EMPTY"))
    {
        return MEDICINE_RESULT_OK;
    }

    if ('\0' == payload[0])
    {
        return MEDICINE_RESULT_PARSE_ERROR;
    }

    record_text = payload;
    while (NULL != record_text)
    {
        uint32_t field_count;
        uint16_t quantity;
        char quantity_text[4];
        int quantity_length;

        if (record_count >= MEDICINE_MAX_RECORDS)
        {
            out_text[0] = '\0';
            return MEDICINE_RESULT_PARSE_ERROR;
        }

        next_record = strstr(record_text, "||");
        if (NULL != next_record)
        {
            *next_record = '\0';
            next_record += 2;
        }

        field_count = split_fields(record_text, fields, 4U);
        if ((4U != field_count) ||
            ('\0' == fields[0][0]) ||
            ('\0' == fields[2][0]) ||
            ('\0' == fields[3][0]) ||
            (!parse_quantity_value(fields[1], false, &quantity)))
        {
            out_text[0] = '\0';
            return MEDICINE_RESULT_PARSE_ERROR;
        }

        for (uint32_t field_index = 0U; field_index < 4U; field_index++)
        {
            for (char * ch = fields[field_index]; '\0' != *ch; ch++)
            {
                if ('"' == *ch)
                {
                    *ch = '\'';
                }
            }
        }

        quantity_length = snprintf(quantity_text,
                                   sizeof(quantity_text),
                                   "%u",
                                   (unsigned int) quantity);
        if ((quantity_length <= 0) || ((size_t) quantity_length >= sizeof(quantity_text)))
        {
            out_text[0] = '\0';
            return MEDICINE_RESULT_PARSE_ERROR;
        }

        records[record_count].name = fields[0];
        records[record_count].production_date = fields[2];
        records[record_count].shelf_life = fields[3];
        records[record_count].quantity = quantity;
        records[record_count].name_width = utf8_display_width(fields[0]);
        records[record_count].quantity_width = (size_t) quantity_length;
        records[record_count].production_date_width = utf8_display_width(fields[2]);

        if (records[record_count].name_width > max_name_width)
        {
            max_name_width = records[record_count].name_width;
        }
        if (records[record_count].quantity_width > max_quantity_width)
        {
            max_quantity_width = records[record_count].quantity_width;
        }
        if (records[record_count].production_date_width > max_production_date_width)
        {
            max_production_date_width = records[record_count].production_date_width;
        }
        record_count++;

        record_text = next_record;
        if ((NULL != record_text) && ('\0' == record_text[0]))
        {
            out_text[0] = '\0';
            return MEDICINE_RESULT_PARSE_ERROR;
        }
    }

    for (uint8_t record_index = 0U; record_index < record_count; record_index++)
    {
        char quantity_text[4];
        int quantity_length = snprintf(quantity_text,
                                       sizeof(quantity_text),
                                       "%u",
                                       (unsigned int) records[record_index].quantity);
        bool appended = true;

        if ((quantity_length <= 0) || ((size_t) quantity_length >= sizeof(quantity_text)))
        {
            out_text[0] = '\0';
            return MEDICINE_RESULT_PARSE_ERROR;
        }

        if (record_index > 0U)
        {
            appended = inventory_append(out_text, out_size, &used, "\r\n", 2U);
        }
        appended = appended && inventory_append(out_text,
                                                 out_size,
                                                 &used,
                                                 records[record_index].name,
                                                 strlen(records[record_index].name));
        appended = appended && inventory_append_spaces(out_text,
                                                        out_size,
                                                        &used,
                                                        (max_name_width -
                                                         records[record_index].name_width) + 2U);
        appended = appended && inventory_append(out_text,
                                                 out_size,
                                                 &used,
                                                 quantity_text,
                                                 (size_t) quantity_length);
        appended = appended && inventory_append_spaces(out_text,
                                                        out_size,
                                                        &used,
                                                        (max_quantity_width -
                                                         records[record_index].quantity_width) + 2U);
        appended = appended && inventory_append(out_text,
                                                 out_size,
                                                 &used,
                                                 records[record_index].production_date,
                                                 strlen(records[record_index].production_date));
        appended = appended && inventory_append_spaces(out_text,
                                                        out_size,
                                                        &used,
                                                        (max_production_date_width -
                                                         records[record_index].production_date_width) + 2U);
        appended = appended && inventory_append(out_text,
                                                 out_size,
                                                 &used,
                                                 records[record_index].shelf_life,
                                                 strlen(records[record_index].shelf_life));

        if (!appended)
        {
            out_text[0] = '\0';
            return MEDICINE_RESULT_BUFFER_TOO_SMALL;
        }
    }

    return MEDICINE_RESULT_OK;
}

const char * medicine_result_text(medicine_result_t result)
{
    switch (result)
    {
        case MEDICINE_RESULT_OK:
            return "OK";

        case MEDICINE_RESULT_BUSY:
            return "BUSY";

        case MEDICINE_RESULT_NETWORK_ERROR:
            return "NETWORK_ERROR";

        case MEDICINE_RESULT_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";

        case MEDICINE_RESULT_NOT_FOUND:
            return "NOT_FOUND";

        case MEDICINE_RESULT_INSUFFICIENT:
            return "INSUFFICIENT";

        case MEDICINE_RESULT_RANGE_ERROR:
            return "RANGE_ERROR";

        case MEDICINE_RESULT_SERVER_ERROR:
            return "SERVER_ERROR";

        case MEDICINE_RESULT_PARSE_ERROR:
            return "PARSE_ERROR";

        case MEDICINE_RESULT_BUFFER_TOO_SMALL:
            return "BUFFER_TOO_SMALL";

        default:
            return "UNKNOWN_ERROR";
    }
}
