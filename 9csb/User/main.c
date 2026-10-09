#include "stm32f10x.h"
#include "Delay.h"
#include "robot.h"
#include "Serial.h"
#include "timer.h"
#include "Buzzer.h"

/*
 * ESP32-S3 -> STM32F103C8T6 人跟随控制
 *
 * 协议：AA 55 LEFT RIGHT CHECK
 * LEFT/RIGHT 为 int8_t，-100~100。
 * CHECK = AA ^ 55 ^ LEFT ^ RIGHT
 *
 * USART1：PA9 TX，PA10 RX，115200 8N1
 *
 * 调试阶段暂时关闭超声波避障，仅保留通信超时停车保护。
 * 收到首帧校验正确的数据时，蜂鸣器提示一次。
 */

#define ESP_CONTROL_TIMEOUT_MS 700

static int8_t g_left_speed = 0;
static int8_t g_right_speed = 0;
static uint16_t g_control_age_ms = ESP_CONTROL_TIMEOUT_MS;
static uint32_t g_valid_frame_count = 0;
static uint16_t g_rx_beep_ms = 0;
static uint8_t g_rx_beep_done = 0;

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
					g_valid_frame_count++;
					/* 首次收到校验正确的数据帧时，蜂鸣器响约100ms。 */
					if(!g_rx_beep_done)
					{
						g_rx_beep_done = 1;
						g_rx_beep_ms = 100;
					}
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
	uint16_t debug_ms = 0;

	Timerx_Init(5000,7199);
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

	Buzzer_Init();
	Serial_Init();
	robot_Init();

	robot_set_signed_speed(0,0);

	Serial_Printf("\r\nESP32 person-follow controller ready\r\n");
	Serial_Printf("USART1 PA9/PA10 115200 8N1\r\n");

	while(1)
	{
		parse_esp_command();

		/* 非阻塞蜂鸣提示：不延迟主循环，避免影响串口接收和电机控制。 */
		if(g_rx_beep_ms > 0)
		{
			Buzzer_ON();
			g_rx_beep_ms--;
		}
		else
		{
			Buzzer_OFF();
		}

		if(g_control_age_ms < ESP_CONTROL_TIMEOUT_MS)
		{
			g_control_age_ms++;
		}

		/* 本次调试暂时关闭超声波避障；通信超时保护仍然保留。 */
		if(g_control_age_ms >= ESP_CONTROL_TIMEOUT_MS)
		{
			robot_set_signed_speed(0,0);
		}
		else
		{
			robot_set_signed_speed(g_left_speed,g_right_speed);
		}

		/* 每约 500ms 从 USART1_TX(PA9) 输出一次诊断，确认收到帧及停车原因。 */
		debug_ms++;
		if(debug_ms >= 500)
		{
			uint8_t stop_reason = 0;
			if(g_control_age_ms >= ESP_CONTROL_TIMEOUT_MS)
			{
				stop_reason = 1; /* 串口超时 */
			}
			Serial_Printf("DBG rx=%lu L=%d R=%d age=%u stop=%u\r\n",
			              (unsigned long)g_valid_frame_count,
			              (int)g_left_speed, (int)g_right_speed,
			              (unsigned int)g_control_age_ms,
			              (unsigned int)stop_reason);
			debug_ms = 0;
		}

		Delay_ms(1);

	}
}
