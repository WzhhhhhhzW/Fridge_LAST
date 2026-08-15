/*
 * PID_control.h
 *
 *  Created on: 2026年2月15日
 *      Author: lenovo
 */

#ifndef PID_CONTROL_H_
#define PID_CONTROL_H_

//单极PID结构体
typedef struct
{
    float kp, ki, kd; //三个系数
    float error, lastError; //误差、上次误差
    float integral, maxIntegral; //积分、积分限幅
    float output, maxOutput; //输出、输出限幅
}PID;
//串级PID的结构体，包含两个单级PID
typedef struct
{
    PID inner; //内环
    PID outer; //外环
    float output; //串级输出，等于inner.output
}CascadePID;

void PID_Init(PID *pid, float p, float i, float d, float maxI, float maxOut);
void PID_Calc(PID *pid, float reference, float feedback);
void PID_CascadeCalc(CascadePID *pid, float outerRef, float outerFdb, float innerFdb);


#endif /* PID_CONTROL_H_ */
