#include "stm32f10x.h"
#include "PWM.h"
#include "Delay.h"

void robot_Init(void)
{
	PWM_Init();
}

void robot_speed(uint8_t left1_speed,uint8_t left2_speed,uint8_t right1_speed,uint8_t right2_speed)
{
	TIM_SetCompare1(TIM4,left1_speed);
	TIM_SetCompare2(TIM4,left2_speed);
	TIM_SetCompare3(TIM4,right1_speed);
	TIM_SetCompare4(TIM4,right2_speed);
}

void robot_set_signed_speed(int8_t left_speed, int8_t right_speed)
{
	if(left_speed > 100) left_speed = 100;
	if(left_speed < -100) left_speed = -100;
	if(right_speed > 100) right_speed = 100;
	if(right_speed < -100) right_speed = -100;

	if(left_speed >= 0)
	{
		TIM_SetCompare1(TIM4,(uint8_t)left_speed);
		TIM_SetCompare2(TIM4,0);
	}
	else
	{
		TIM_SetCompare1(TIM4,0);
		TIM_SetCompare2(TIM4,(uint8_t)(-left_speed));
	}

	if(right_speed >= 0)
	{
		TIM_SetCompare3(TIM4,(uint8_t)right_speed);
		TIM_SetCompare4(TIM4,0);
	}
	else
	{
		TIM_SetCompare3(TIM4,0);
		TIM_SetCompare4(TIM4,(uint8_t)(-right_speed));
	}
}

void makerobo_run(int8_t speed,uint16_t time)
{
	if(speed > 100) speed = 100;
	if(speed < 0) speed = 0;
	robot_speed(speed,0,speed,0);
	Delay_ms(time);
}

void makerobo_brake(uint16_t time)
{
	robot_speed(0,0,0,0);
	Delay_ms(time);
}

void makerobo_Left(int8_t speed,uint16_t time)
{
	if(speed > 100) speed = 100;
	if(speed < 0) speed = 0;
	robot_speed(0,0,speed,0);
	Delay_ms(time);
}

void makerobo_Spin_Left(int8_t speed,uint16_t time)
{
	if(speed > 100) speed = 100;
	if(speed < 0) speed = 0;
	robot_speed(0,speed,speed,0);
	Delay_ms(time);
}

void makerobo_Right(int8_t speed,uint16_t time)
{
	if(speed > 100) speed = 100;
	if(speed < 0) speed = 0;
	robot_speed(speed,0,0,0);
	Delay_ms(time);
}

void makerobo_Spin_Right(int8_t speed,uint16_t time)
{
	if(speed > 100) speed = 100;
	if(speed < 0) speed = 0;
	robot_speed(speed,0,0,speed);
	Delay_ms(time);
}

void makerobo_back(int8_t speed,uint16_t time)
{
	if(speed > 100) speed = 100;
	if(speed < 0) speed = 0;
	robot_speed(0,speed,0,speed);
	Delay_ms(time);
}
