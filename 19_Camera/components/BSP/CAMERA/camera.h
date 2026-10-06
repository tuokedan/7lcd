#ifndef __CAMERA_H__
#define __CAMERA_H__


#include <stdint.h>

/* 引脚声明 */
#define CAM_PIN_PWDN     GPIO_NUM_10
#define CAM_PIN_RESET    GPIO_NUM_9
#define CAM_PIN_XCLK     GPIO_NUM_NC
#define CAM_PIN_SIOD     GPIO_NUM_39
#define CAM_PIN_SIOC     GPIO_NUM_38
#define CAM_PIN_D7       GPIO_NUM_18
#define CAM_PIN_D6       GPIO_NUM_17
#define CAM_PIN_D5       GPIO_NUM_16
#define CAM_PIN_D4       GPIO_NUM_15
#define CAM_PIN_D3       GPIO_NUM_7
#define CAM_PIN_D2       GPIO_NUM_6
#define CAM_PIN_D1       GPIO_NUM_5
#define CAM_PIN_D0       GPIO_NUM_4
#define CAM_PIN_VSYNC    GPIO_NUM_14
#define CAM_PIN_HREF     GPIO_NUM_3
#define CAM_PIN_PCLK     GPIO_NUM_45

#define CAM_PWDN(x)     x ? gpio_set_level(CAM_PIN_PWDN, 1)  : gpio_set_level(CAM_PIN_PWDN, 0) 
#define CAM_RST(x)      x ? gpio_set_level(CAM_PIN_RESET, 1) : gpio_set_level(CAM_PIN_RESET, 0) 

/* 函数声明 */
void camera_init(void);                 /* 摄像头初始化 */
void camera_show(uint16_t x, uint16_t y);  /* 摄像头显示 */
#endif
