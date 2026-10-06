#ifndef __MYLCD_H_
#define __MYLCD_H_

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#define LCD_WIDTH   240
#define LCD_HEIGHT  240

/* ESP32-S3 V1.4 原理图对应的 LCD 引脚 */
#define LCD_RST(x)  gpio_set_level(GPIO_NUM_2,  (x) ? 1 : 0)
#define LCD_DC(x)   gpio_set_level(GPIO_NUM_41, (x) ? 1 : 0)
#define LCD_CS(x)   gpio_set_level(GPIO_NUM_45, (x) ? 1 : 0)

/*
 * 原理图没有独立 LCD 背光 GPIO。
 * GPIO42 是 LCD_CLK，不能再作为背光控制。
 * 背光由显示屏接口硬件供电，因此这里不操作 GPIO42。
 */

#define WHITE       0xFFFF
#define BLACK       0x0000
#define RED         0xF800
#define GREEN       0x07E0
#define BLUE        0x001F
#define MAGENTA     0xF81F
#define YELLOW      0xFFE0
#define CYAN        0x07FF
#define BROWN       0xBC40
#define BRRED       0xFC07
#define GRAY        0x8430
#define DARKBLUE    0x01CF
#define LIGHTBLUE   0x7D7C
#define GRAYBLUE    0x5458
#define LIGHTGREEN  0x841F
#define LGRAY       0xC618
#define LGRAYBLUE   0xA651
#define LBBLUE      0x2B12

void lcd_write_cmd(uint8_t cmd);
void lcd_write_data(uint8_t data);
void lcd_write_data16(uint16_t data);
void lcd_write_datan(const uint8_t *data, uint16_t length);
void lcd_hard_reset(void);
void lcd_set_window(uint16_t xstar, uint16_t ystar, uint16_t xend, uint16_t yend);
void lcd_clear(uint16_t color);
void lcd_init(void);
void lcd_set_cursor(uint16_t xpos, uint16_t ypos);
void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void lcd_show_char(uint8_t line, uint8_t column, uint8_t chr,
                  uint16_t fontcolor, uint16_t backgroundcolor);
void lcd_show_string(uint8_t line, uint8_t column, char *string,
                    uint16_t fontcolor, uint16_t backgroundcolor);
void lcd_show_num(uint8_t line, uint8_t column, uint32_t number, uint8_t length,
                 uint16_t fontcolor, uint16_t backgroundcolor);
void lcd_show_hexnum(uint8_t line, uint8_t column, uint32_t number, uint8_t length,
                    uint16_t fontcolor, uint16_t backgroundcolor);
void lcd_show_float(uint8_t line, uint8_t column, float number, uint8_t length,
                   uint16_t fontcolor, uint16_t backgroundcolor);
void lcd_show_picture(uint8_t *img);

#endif
