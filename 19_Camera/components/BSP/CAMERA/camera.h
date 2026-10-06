#ifndef __CAMERA_H__
#define __CAMERA_H__

#include <stdint.h>
#include "driver/gpio.h"

/*
 * YD-ESP32-S3 V1.4 + 当前摄像头接线
 *
 * 下面这组引脚沿用之前已经验证过的摄像头工程。
 * XCLK/PWDN 在当前硬件上未由 ESP32-S3 GPIO 输出，因此使用 NC。
 */
#define CAM_PIN_PWDN     GPIO_NUM_NC
#define CAM_PIN_RESET    GPIO_NUM_15
#define CAM_PIN_XCLK     GPIO_NUM_NC
#define CAM_PIN_SIOD     GPIO_NUM_6
#define CAM_PIN_SIOC     GPIO_NUM_4

#define CAM_PIN_D7       GPIO_NUM_10
#define CAM_PIN_D6       GPIO_NUM_9
#define CAM_PIN_D5       GPIO_NUM_46
#define CAM_PIN_D4       GPIO_NUM_3
#define CAM_PIN_D3       GPIO_NUM_8
#define CAM_PIN_D2       GPIO_NUM_18
#define CAM_PIN_D1       GPIO_NUM_17
#define CAM_PIN_D0       GPIO_NUM_16

#define CAM_PIN_VSYNC    GPIO_NUM_5
#define CAM_PIN_HREF     GPIO_NUM_7
#define CAM_PIN_PCLK     GPIO_NUM_11

#define CAM_PWDN(x)     ((void)0)
#define CAM_RST(x)      gpio_set_level(CAM_PIN_RESET, (x) ? 1 : 0)

void camera_init(void);
void camera_show(uint16_t x, uint16_t y);

#endif
