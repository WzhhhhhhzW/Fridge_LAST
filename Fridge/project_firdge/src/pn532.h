#ifndef PN532_H
#define PN532_H

#include "hal_data.h"

#include <stdbool.h>
#include <stdint.h>

#define PN532_UID_MAX_BYTES (7U)

fsp_err_t pn532_init(void);
void pn532_process(void);
bool pn532_ready(void);
bool pn532_card_present(void);
uint32_t pn532_scan_generation(void);
bool pn532_take_uid(uint8_t * uid, uint8_t * uid_length);
void pn532_suspend(void);
void pn532_resume(void);
void pn532_tick_1ms(void);

void pn532_spi_callback(spi_callback_args_t * p_args);
void spi_callback(spi_callback_args_t * p_args);

#endif /* PN532_H */
