#include "variable.h"

uint8_t rx_byte_uart2 = 0;//OLED
uint8_t rx_byte_uart7 = 0;//调试串口
uint8_t tx_buf[TX_BUF_SIZE];
uint8_t tx_buf1[TX_BUF_SIZE];
uint8_t tx_buf2[TX_BUF_SIZE];
uint8_t tx_buf3[TX_BUF_SIZE];
uint8_t tx_buf4[TX_BUF_SIZE];
uint8_t tx_buf5[TX_BUF_SIZE];
uint8_t tx_buf10[TX_BUF_SIZE];
int set_temp = 1;//需求温度
uint8_t fan_state = 0;//风扇开关 0为关 1为开，2为高速档
uint8_t Water_cool_State =0;//水冷系统状态 0关闭 1开启
bool door_state = false;//门的开关 0为关 1为开
bool cool_state = false;//控制制冷片开关
fsp_err_t err = FSP_SUCCESS;
uint32_t len = 0;
uint32_t len1 = 0;
uint32_t len2 = 0;
uint32_t len3 = 0;
uint32_t len4 = 0;
uint32_t len5 = 0;
uint32_t len10 = 0;

float temperature = 0.0f;//当前温度
int cnt = 0;//

PID Colder;//定义制冷片控制结构体
float Colder_P = 0.8f;
float Colder_I = 0.01f;
float Colder_D = 0.0f;

PID Fan;//定义风扇控制结构体
float Fan_P = 0.1f;
float Fan_I = 0.1f;
float Fan_D = 0.1f;
uint8_t Medical[1] = {0};

//定时器中断延时控制标志位
volatile int OLED_Count = 0;//控制串口屏收发延时
volatile int OLED_Inner_Count = 0;//控制串口屏收发函数内部延时
volatile int OLED_Send_Mode = 0;//

volatile uint8_t Flag = 0;



