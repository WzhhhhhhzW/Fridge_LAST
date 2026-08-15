#include "variable.h"

void Counter_Timer_Init()//定时器延时初始化
{
    R_GPT_Open(&Counter_Timer_ctrl, &Counter_Timer_cfg);
    R_GPT_Start(&Counter_Timer_ctrl);
}


void Counter_Timer_Callback(timer_callback_args_t *p_args)//定时器中断回调函数
{
    (void) p_args;
    OLED_Count++;//每1ms自增1
    //串口屏收发控制
    switch(OLED_Count)
    {
        case 10://每轮循环第十ms模式设定为设定温度
            OLED_Send_Mode = 0;
            break;
        case 20://发送当前温度数据
            OLED_Send_Mode = 1;
            break;
        case 30://发送风扇控制数据
            OLED_Send_Mode = 2;
            break;
        case 40://发送制冷片开关数据
            OLED_Send_Mode = 3;
            break;
        case 50://发送当前功率数据
            OLED_Send_Mode = 4;
            break;
        case 70://发送当前水冷状态数据
            OLED_Send_Mode = 5;
            OLED_Count = 0;//OLED计数值复位
            break;
        default:
            break;
    }
}
