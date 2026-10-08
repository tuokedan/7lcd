#include "stm32f10x.h"
#include "Delay.h"
#include "robot.h"
#include "UltrasonicWave.h"
#include "Serial.h"
#include "timer.h"
#include "Buzzer.h"
#include "Servo.h"

/*
 * ESP32-S3 -> STM32F103C8T6 人跟随控制
 *
 * 协议：AA 55 LEFT RIGHT CHECK
 * LEFT/RIGHT 为 int8_t，-100~100。
 * CHECK = AA ^ 55 ^ LEFT ^ RIGHT
 *
 * USART1：PA9 TX，PA10 RX，115200 8N1
 *
 * 优先级：
 * 1. ESP32 通信超时 -> 停车
 * 2. 前方超声波 < 60 cm -> 停车
 * 3. ESP32 人跟随左右轮速度
 */

#define ESP_CONTROL_TIMEOUT_MS 700
#define OBSTACLE_STOP_CM_X10 600

static int8_t g_left_speed = 0;
static int8_t g_right_speed = 0;
static uint16_t g_control_age_ms = ESP_CONTROL_TIMEOUT_MS;
static int g_front_distance_x10 = 0;

static void parse_esp_command(void)
{
	static uint8_t state = 0;
	static uint8_t left_byte = 0;
	static uint8_t right_byte = 0;
	uint8_t byte_value;

	while(Serial_ReadByte(&byte_value))
	{
		switch(state)
		{
			case 0:
				if(byte_value == 0xAA) state = 1;
				break;

			case 1:
				if(byte_value == 0x55)
				{
					state = 2;
				}
				else if(byte_value != 0xAA)
				{
					state = 0;
				}
				break;

			case 2:
				left_byte = byte_value;
				state = 3;
				break;

			case 3:
				right_byte = byte_value;
				state = 4;
				break;

			case 4:
				if(byte_value == (uint8_t)(0xAA ^ 0x55 ^ left_byte ^ right_byte))
				{
					g_left_speed = (int8_t)left_byte;
					g_right_speed = (int8_t)right_byte;
					g_control_age_ms = 0;
				}
				state = 0;
				break;

			default:
				state = 0;
				break;
		}
	}
}

int main(void)
{
	uint16_t tick_ms = 0;

	Timerx_Init(5000,7199);
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

	Buzzer_Init();
	UltrasonicWave_Init();
	Serial_Init();
	robot_Init();
	Servo_Init();

	/* 超声波舵机固定朝正前方，只做安全停车，不再自动左右扫描。 */
	Servo_SetAngle(90);
	robot_set_signed_speed(0,0);

	Serial_Printf("
ESP32 person-follow controller ready\r
");
	Serial_Printf("USART1 PA9/PA10 115200 8N1\r
");

	while(1)
	{
		parse_esp_command();

		/* 每 100 ms 触发一次新的前方测距。 */
		if(tick_ms == 0)
		{
			g_front_distance_x10 = UltrasonicWave_StartMeasure();
		}

		if(g_control_age_ms < ESP_CONTROL_TIMEOUT_MS)
		{
			g_control_age_ms++;
		}

		/*
		 * 安全优先：
		 * 通信中断、没有检测到人（ESP32 会发送 0/0）、或超声波发现障碍，
		 * 最终都会让车辆停下。
		 */
		if(g_control_age_ms >= ESP_CONTROL_TIMEOUT_MS)
		{
			robot_set_signed_speed(0,0);
		}
		else if(g_front_distance_x10 > 0 &&
		        g_front_distance_x10 < OBSTACLE_STOP_CM_X10)
		{
			robot_set_signed_speed(0,0);
		}
		else
		{
			robot_set_signed_speed(g_left_speed,g_right_speed);
		}

		Delay_ms(1);

		tick_ms++;
		if(tick_ms >= 100)
		{
			tick_ms = 0;
		}
	}
}
