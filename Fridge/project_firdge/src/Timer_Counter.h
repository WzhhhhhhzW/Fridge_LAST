#ifndef TIMER_COUNTER_H_
#define TIMER_COUNTER_H_

#include "hal_data.h"
void Counter_Timer_Init();//定时器延时初始化
void Counter_Timer_Callback(timer_callback_args_t *p_args);//定时器中断回调

#endif /* TIMER_COUNTER_H_ */
