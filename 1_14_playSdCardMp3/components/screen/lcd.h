// Copyright 2019 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _LCD_H_
#define _LCD_H_


#include "driver/gpio.h"
#include "driver/spi_master.h"



#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

//LCD引脚定义
#define LCD_RST   GPIO_NUM_2
#define LCD_CLK   GPIO_NUM_42  //时钟
#define LCD_DC    GPIO_NUM_41
#define LCD_CS    GPIO_NUM_45
#define LCD_MOSI  GPIO_NUM_48
#define LCD_MISO  GPIO_NUM_47  //SPI读数据
#define LCD_BK    -1


#define DEMO_SPI_MAX_TRANFER_SIZE (240 * 320 * 2)



void lcd_init();
void lcd_update();
void lcd_clear();
uint8_t *lcd_GetBuff();
int lcd_getLineMaxByte(int zk_num);
uint16_t lcd_GetWidth();
uint16_t lcd_GetHeight();

#ifdef __cplusplus
}
#endif


#endif
