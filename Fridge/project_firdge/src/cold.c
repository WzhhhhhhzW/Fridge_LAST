/*
 * cold.c
 *
 *  Created on: 2026年2月24日
 *      Author: lenovo
 */
#include "cold.h"

void Colder_Init()
{
    R_GPT_Open(&Cold_Maker_ctrl, &Cold_Maker_cfg);
    R_GPT_Start(&Cold_Maker_ctrl);
    /* FSP timer default is 50%; force the cooling outputs off immediately. */
    Colder_DutySet(0U);
}


void Colder_DutySet(uint8_t duty)
{
    timer_info_t info;
    uint32_t current_period_counts;
    uint32_t duty_cycle_counts;

    if (duty > 100)
        duty = 100; //限制占空比范围：0~100

    /* 获得GPT的信息 */
    R_GPT_InfoGet(&Cold_Maker_ctrl, &info);

    /* 获得计时器一个周期需要的计数次数 */
    current_period_counts = info.period_counts;

    /* 根据占空比和一个周期的计数次数计算GTCCR寄存器的值 */
    duty_cycle_counts = (uint32_t)(((uint64_t) current_period_counts *(100 - duty)) / 100);

    /* 最后调用FSP库函数设置占空比 */
    R_GPT_DutyCycleSet(&Cold_Maker_ctrl, duty_cycle_counts, GPT_IO_PIN_GTIOCA);//设置左侧制冷片占空比
    R_GPT_DutyCycleSet(&Cold_Maker_ctrl, duty_cycle_counts, GPT_IO_PIN_GTIOCB);//设置右侧制冷片占空比
}
