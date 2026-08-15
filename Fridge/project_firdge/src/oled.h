#ifndef OLED_H_
#define OLED_H_

#include <stdbool.h>
#include "hal_data.h"

void OLED_Start();
void OLED_SetHealthAdvice(const char * text);
void OLED_ShowDeepseekPage(void);
void OLED_SetHealthResult(const char * diagnosis, const char * medicine);
void OLED_SetInventoryText(const char * text);
void OLED_SetFanRpm(uint32_t fan1_rpm, uint32_t fan2_rpm);
void OLED_SetFanWaterState(uint8_t fan_state, bool water_enabled);
void OLED_SetPreheatActive(bool active);
void OLED_SetDoorClosed(bool closed);
void OLED_NotifyTxComplete(void);
bool OLED_IsInventoryTxProtected(void);
bool OLED_IsTxBusy(void);
fsp_err_t OLED_TryWrite(uint8_t * data, uint32_t length);

#endif /* OLED_H_ */
