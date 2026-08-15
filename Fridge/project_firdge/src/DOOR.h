/*
 * DOOR.h
 *
 *  Created on: 2026年3月10日
 *      Author: lenovo
 */

#ifndef DOOR_H_
#define DOOR_H_

#include <stdbool.h>
#include <stdint.h>

void Door_Init(void);
void door_open(void);
void door_close(void);
bool door_unlock_request(void);
void door_process(void);
void door_tick_1ms(uint32_t now_ms);
bool door_unlock_active(void);
bool door_is_closed(void);
#endif /* DOOR_H_ */
