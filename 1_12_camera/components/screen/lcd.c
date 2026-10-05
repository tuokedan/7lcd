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

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp32s3/rom/lldesc.h"
#include "soc/system_reg.h"
#include "esp_log.h"
#include "spi_bus.h"
#include "lcd.h"
#include "zk.h"
#include "st7789.h"

#include "screen_driver.h"

static const char *TAG = "lcd";
uint16_t *lcd_data_buf = NULL;

static bool s_lcd_inited = false;
static scr_info_t s_lcd_info;


//填充LCD上的一个点
void SetBitColor(u16 x,u16 y,u16 color)
{
    if(x>=s_lcd_info.width) return;
    if(y>=s_lcd_info.height) return;

    *(lcd_data_buf+y*s_lcd_info.width+x)= color;
}

int lcd_getLineMaxByte(int zk_num)
{
    if(zk_num==32)
    {
        return (s_lcd_info.width)/16+1;
    }
    else if(zk_num==24)
    {
        return (s_lcd_info.width)/12+1;
    }

    return (s_lcd_info.width)/8+1;
}

//把显存数据更新到显示屏
void lcd_update()
{
    int y = 0;
    int line=40;
    if(lcd_data_buf==NULL) return;

    while(y<s_lcd_info.height)
    {
      if(y+line>s_lcd_info.height) line=s_lcd_info.height-y;
      lcd_st7789_draw_bitmap(0, y, s_lcd_info.width, line, (uint16_t *)(lcd_data_buf+y*s_lcd_info.width));
      y+=line;
    }

//    lcd_st7789_draw_bitmap(0, 0, s_lcd_info.width, s_lcd_info.height, (uint16_t *)lcd_data_buf);
}

uint16_t lcd_GetWidth()
{
    return s_lcd_info.width;
}

uint16_t lcd_GetHeight()
{
    return s_lcd_info.height;
}

void lcd_clear()
{
    memset((void*)lcd_data_buf, 0xff, s_lcd_info.width * s_lcd_info.height * sizeof(uint16_t));
}

uint8_t *lcd_GetBuff()
{
    return (uint8_t *)lcd_data_buf;
}

void lcd_init(void)
{
    spi_config_t bus_conf = {
        .miso_io_num = LCD_MISO,
        .mosi_io_num = LCD_MOSI,
        .sclk_io_num = LCD_CLK,
        .max_transfer_sz = DEMO_SPI_MAX_TRANFER_SIZE,
    };
    spi_bus_handle_t spi_bus = spi_bus_create(SPI2_HOST, &bus_conf);

    if (spi_bus == NULL) {
        ESP_LOGE(TAG, "spi_bus2 create failed");
    }

    scr_interface_spi_config_t spi_lcd_cfg = {
        .spi_bus = spi_bus,
        .pin_num_cs = LCD_CS,
        .pin_num_dc = LCD_DC,
        .clk_freq = 40000000,
        .swap_data = false,
    };

    scr_interface_driver_t *iface_drv;
    scr_interface_create(SCREEN_IFACE_SPI, &spi_lcd_cfg, &iface_drv);

    scr_controller_config_t lcd_cfg = {
        .interface_drv = iface_drv,
        .pin_num_rst = LCD_RST,
        .pin_num_bckl = LCD_BK,
        .rst_active_level = 0,
        .bckl_active_level = 1,
        .offset_hor = 0,
        .offset_ver = 0,
        .width = 240,
        .height = 320,
        .rotate = SCR_DIR_BTLR,
    };

    esp_err_t ret = lcd_st7789_init(&lcd_cfg);

    if (ESP_OK != ret) {
        return;
        ESP_LOGE(TAG, "screen initialize failed");
    }

    lcd_st7789_get_info(&s_lcd_info);
    ESP_LOGI(TAG, "Screen name:%s | width:%d | height:%d", s_lcd_info.name, s_lcd_info.width, s_lcd_info.height);

    lcd_st7789_set_invert(false);
    vTaskDelay(pdMS_TO_TICKS(100));

    //申请LCD显存
    lcd_data_buf = (uint16_t *)heap_caps_calloc(s_lcd_info.width * s_lcd_info.height, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if(lcd_data_buf==NULL) printf("*******************lcd_data_buf is NULL*************************");
    else memset((void*)lcd_data_buf, 0xff, s_lcd_info.width * s_lcd_info.height*sizeof(uint16_t));

    //清屏
    lcd_clear();
    lcd_update();

    s_lcd_inited = true;
}


