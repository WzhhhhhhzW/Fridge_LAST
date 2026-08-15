/*
 * variable.h
 *
 *  Created on: 2026年2月14日
 *      Author: lenovo
 */

#ifndef VARIABLE_H_
#define VARIABLE_H_

#include "hal_data.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "Timer_Counter.h"//定时器延时控制
#include "system_tick.h"//功率定时器模块
#include "LM35.h"//TMP235温度传感器（保留原文件名兼容现有工程）
#include "fan_control.h"//包含风扇控制的头文件
#include "oled.h"//包含串口屏控制的头文件
#include "PID_control.h"//包含PID控制的头文件
#include "cold.h"//包含制冷片控制的头文件
#include "DOOR.h"//包含舵机控制的文件


#define TX_BUF_SIZE 320
extern uint8_t rx_byte_uart2;//OLED
extern uint8_t rx_byte_uart7;//调试串口
extern uint8_t tx_buf[TX_BUF_SIZE];
extern uint8_t tx_buf1[TX_BUF_SIZE];
extern uint8_t tx_buf2[TX_BUF_SIZE];
extern uint8_t tx_buf3[TX_BUF_SIZE];
extern uint8_t tx_buf4[TX_BUF_SIZE];
extern uint8_t tx_buf5[TX_BUF_SIZE];
extern uint8_t tx_buf10[TX_BUF_SIZE];
extern int set_temp;//需求温度
extern uint8_t fan_state;
extern uint8_t Water_cool_State;//水冷系统状态 0关闭 1开启
extern bool door_state;
extern bool cool_state;
extern fsp_err_t err;
extern uint32_t len;
extern uint32_t len1;
extern uint32_t len2;
extern uint32_t len3;
extern uint32_t len4;
extern uint32_t len5;
extern uint32_t len10;


extern float temperature;//当前温度
extern int cnt;

extern PID Colder;//定义制冷片控制结构体
extern float Colder_P;
extern float Colder_I;
extern float Colder_D;

extern PID Fan;//定义风扇控制结构体
extern float Fan_P;
extern float Fan_I;
extern float Fan_D;
extern uint8_t Medical[1];

extern volatile uint8_t Flag ;

//定时器中断延时控制标志位
extern volatile int OLED_Count;//控制串口屏收发延时
extern volatile int OLED_Inner_Count;//控制串口屏收发函数内部延时
extern volatile int OLED_Send_Mode;

#endif /* VARIABLE_H_ */
