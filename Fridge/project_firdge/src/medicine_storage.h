#ifndef MEDICINE_STORAGE_H_
#define MEDICINE_STORAGE_H_

#include "hal_data.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MEDICINE_MAX_RECORDS        32U
#define MEDICINE_NAME_MAX           48U
#define MEDICINE_DATE_MAX           16U
#define MEDICINE_SHELF_LIFE_MAX     24U
#define MEDICINE_EFFICACY_MAX       160U
#define MEDICINE_QUANTITY_MAX       999U
#define MEDICINE_INVENTORY_TEXT_MAX (12U * 1024U)

typedef enum e_medicine_result
{
    MEDICINE_RESULT_OK = 0,
    MEDICINE_RESULT_BUSY,
    MEDICINE_RESULT_NETWORK_ERROR,
    MEDICINE_RESULT_INVALID_ARGUMENT,
    MEDICINE_RESULT_NOT_FOUND,
    MEDICINE_RESULT_INSUFFICIENT,
    MEDICINE_RESULT_RANGE_ERROR,
    MEDICINE_RESULT_SERVER_ERROR,
    MEDICINE_RESULT_PARSE_ERROR,
    MEDICINE_RESULT_BUFFER_TOO_SMALL,
} medicine_result_t;

typedef struct st_medicine_record
{
    bool valid;
    uint8_t slot;
    uint16_t quantity;
    char name[MEDICINE_NAME_MAX];
    char production_date[MEDICINE_DATE_MAX];
    char shelf_life[MEDICINE_SHELF_LIFE_MAX];
    char efficacy[MEDICINE_EFFICACY_MAX];
} medicine_record_t;

medicine_result_t medicine_add(const char * name,
                               const char * production_date,
                               const char * shelf_life,
                               const char * efficacy,
                               uint8_t * out_slot);
medicine_result_t medicine_add_with_quantity(const char * name,
                                             const char * production_date,
                                             const char * shelf_life,
                                             const char * efficacy,
                                             uint16_t quantity,
                                             uint8_t * out_slot);
medicine_result_t medicine_set(uint8_t slot,
                               const char * name,
                               const char * production_date,
                               const char * shelf_life,
                               const char * efficacy);
medicine_result_t medicine_set_with_quantity(uint8_t slot,
                                             const char * name,
                                             const char * production_date,
                                             const char * shelf_life,
                                             const char * efficacy,
                                             uint16_t quantity);
medicine_result_t medicine_get(uint8_t slot, medicine_record_t * out_record);
medicine_result_t medicine_delete(uint8_t slot);
medicine_result_t medicine_list(char * out_text, size_t out_size);
medicine_result_t medicine_inventory_list(char * out_text, size_t out_size);
medicine_result_t medicine_quantity_set(uint8_t slot, uint16_t quantity, uint16_t * out_quantity);
medicine_result_t medicine_restock(uint8_t slot, uint16_t count, uint16_t * out_quantity);
medicine_result_t medicine_take(uint8_t slot, uint16_t count, uint16_t * out_quantity);
const char * medicine_result_text(medicine_result_t result);

#endif /* MEDICINE_STORAGE_H_ */
