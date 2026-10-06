#include "lcd.h"
#include "lcdfont.h"
#include "spi.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

#define LCD_BUF_SIZE 11520
#define LCD_TOTAL_BYTES ((size_t)LCD_WIDTH * LCD_HEIGHT * 2)

static uint8_t lcd_buf[LCD_BUF_SIZE];

void lcd_write_cmd(uint8_t cmd)
{
    LCD_DC(0);
    (void)spi2_write_data(&cmd, 1);
}

void lcd_write_data(uint8_t data)
{
    LCD_DC(1);
    (void)spi2_write_data(&data, 1);
}

void lcd_write_data16(uint16_t data)
{
    uint8_t databuf[2] = {
        (uint8_t)(data >> 8),
        (uint8_t)(data & 0xFF)
    };

    LCD_DC(1);
    (void)spi2_write_data(databuf, sizeof(databuf));
}

void lcd_write_datan(const uint8_t *data, uint16_t length)
{
    if (data == NULL || length == 0) {
        return;
    }

    LCD_DC(1);
    (void)spi2_write_data(data, length);
}

void lcd_hard_reset(void)
{
    LCD_RST(0);
    vTaskDelay(pdMS_TO_TICKS(100));

    LCD_RST(1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void lcd_gpio_init(void)
{
    gpio_config_t io = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << GPIO_NUM_41) |
                        (1ULL << GPIO_NUM_2),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* GPIO42 是 LCD_CLK，由 SPI 外设接管，不能配置成背光 GPIO。 */
}

static void lcd_fill_buffer(uint16_t color)
{
    const uint8_t hi = color >> 8;
    const uint8_t lo = color & 0xFF;

    for (size_t i = 0; i < LCD_BUF_SIZE; i += 2) {
        lcd_buf[i] = hi;
        lcd_buf[i + 1] = lo;
    }
}

void lcd_set_window(uint16_t xstar, uint16_t ystar,
                    uint16_t xend, uint16_t yend)
{
    if (xstar >= LCD_WIDTH || ystar >= LCD_HEIGHT) {
        return;
    }

    if (xend >= LCD_WIDTH) {
        xend = LCD_WIDTH - 1;
    }

    if (yend >= LCD_HEIGHT) {
        yend = LCD_HEIGHT - 1;
    }

    lcd_write_cmd(0x2A);
    lcd_write_data16(xstar);
    lcd_write_data16(xend);

    lcd_write_cmd(0x2B);
    lcd_write_data16(ystar);
    lcd_write_data16(yend);

    lcd_write_cmd(0x2C);
}

void lcd_clear(uint16_t color)
{
    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    lcd_fill_buffer(color);

    for (size_t offset = 0; offset < LCD_TOTAL_BYTES; offset += LCD_BUF_SIZE) {
        size_t chunk = LCD_TOTAL_BYTES - offset;
        if (chunk > LCD_BUF_SIZE) {
            chunk = LCD_BUF_SIZE;
        }
        lcd_write_datan(lcd_buf, chunk);
    }
}

void lcd_init(void)
{
    ESP_ERROR_CHECK(spi2_init());

    spi_device_interface_config_t dev_cfg = {
        .clock_source = SPI_CLK_SRC_DEFAULT,
        .clock_speed_hz = 40000000,
        .mode = 0,
        .queue_size = 7,
        .spics_io_num = GPIO_NUM_45,
    };

    ESP_ERROR_CHECK(
        spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi2_handle)
    );

    lcd_gpio_init();

    LCD_DC(1);
    lcd_hard_reset();

    /* 无独立背光 GPIO；背光由显示屏接口硬件供电。 */
    vTaskDelay(pdMS_TO_TICKS(20));

    lcd_write_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_write_cmd(0xB2);
    lcd_write_data(0x0C);
    lcd_write_data(0x0C);
    lcd_write_data(0x00);
    lcd_write_data(0x33);
    lcd_write_data(0x33);

    lcd_write_cmd(0x35);
    lcd_write_data(0x00);

    lcd_write_cmd(0x36);
    lcd_write_data(0x70);
    lcd_write_data(0xA0);

    lcd_write_cmd(0x3A);
    lcd_write_data(0x05);

    lcd_write_cmd(0xB7);
    lcd_write_data(0x35);

    lcd_write_cmd(0xBB);
    lcd_write_data(0x2D);

    lcd_write_cmd(0xC0);
    lcd_write_data(0x2C);

    lcd_write_cmd(0xC2);
    lcd_write_data(0x01);

    lcd_write_cmd(0xC3);
    lcd_write_data(0x15);

    lcd_write_cmd(0xC4);
    lcd_write_data(0x20);

    lcd_write_cmd(0xC6);
    lcd_write_data(0x0F);

    lcd_write_cmd(0xD0);
    lcd_write_data(0xA4);
    lcd_write_data(0xA1);

    lcd_write_cmd(0xD6);
    lcd_write_data(0xA1);

    lcd_write_cmd(0xE0);
    lcd_write_data(0x70);
    lcd_write_data(0x05);
    lcd_write_data(0x0A);
    lcd_write_data(0x0B);
    lcd_write_data(0x0A);
    lcd_write_data(0x27);
    lcd_write_data(0x2F);
    lcd_write_data(0x44);
    lcd_write_data(0x47);
    lcd_write_data(0x37);
    lcd_write_data(0x14);
    lcd_write_data(0x14);
    lcd_write_data(0x29);
    lcd_write_data(0x2F);

    lcd_write_cmd(0xE1);
    lcd_write_data(0x70);
    lcd_write_data(0x07);
    lcd_write_data(0x0C);
    lcd_write_data(0x08);
    lcd_write_data(0x08);
    lcd_write_data(0x04);
    lcd_write_data(0x2F);
    lcd_write_data(0x33);
    lcd_write_data(0x46);
    lcd_write_data(0x18);
    lcd_write_data(0x15);
    lcd_write_data(0x15);
    lcd_write_data(0x2B);
    lcd_write_data(0x2D);

    lcd_write_cmd(0x21);
    lcd_write_cmd(0x29);
    lcd_write_cmd(0x2C);

    lcd_clear(BLACK);
}

void lcd_set_cursor(uint16_t xpos, uint16_t ypos)
{
    lcd_set_window(xpos, ypos, xpos, ypos);
}

void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) {
        return;
    }

    lcd_set_cursor(x, y);
    lcd_write_data16(color);
}

void lcd_show_char(uint8_t line, uint8_t column, uint8_t chr,
                   uint16_t fontcolor, uint16_t backgroundcolor)
{
    if (chr < ' ' || chr > '~' || line == 0 || column == 0) {
        return;
    }

    const uint16_t x0 = (column - 1) * 16;
    const uint16_t y0 = (line - 1) * 32 + 8;

    if (x0 + 15 >= LCD_WIDTH || y0 + 31 >= LCD_HEIGHT) {
        return;
    }

    lcd_set_window(x0, y0, x0 + 15, y0 + 31);

    for (uint8_t i = 0; i < 64; ++i) {
        uint8_t chr_temp = ascii_3216[chr - ' '][i];

        for (uint8_t j = 0; j < 8; ++j) {
            lcd_write_data16(
                (chr_temp & (1U << j)) ? fontcolor : backgroundcolor
            );

            if ((i * 8 + j + 1) % 16 == 0) {
                break;
            }
        }
    }
}

void lcd_show_string(uint8_t line, uint8_t column, char *string,
                     uint16_t fontcolor, uint16_t backgroundcolor)
{
    if (string == NULL) {
        return;
    }

    for (uint8_t i = 0; string[i] != '\0'; ++i) {
        if ((column - 1 + i) * 16 >= LCD_WIDTH) {
            break;
        }

        lcd_show_char(line, column + i, (uint8_t)string[i],
                      fontcolor, backgroundcolor);
    }
}

static uint32_t lcd_pow(uint32_t x, uint32_t y)
{
    uint32_t result = 1;

    while (y--) {
        result *= x;
    }

    return result;
}

void lcd_show_num(uint8_t line, uint8_t column, uint32_t number,
                  uint8_t length, uint16_t fontcolor,
                  uint16_t backgroundcolor)
{
    for (uint8_t i = 0; i < length; ++i) {
        uint32_t divisor = lcd_pow(10, length - i - 1);
        uint8_t digit = (number / divisor) % 10;

        lcd_show_char(line, column + i, digit + '0',
                      fontcolor, backgroundcolor);
    }
}

void lcd_show_hexnum(uint8_t line, uint8_t column, uint32_t number,
                     uint8_t length, uint16_t fontcolor,
                     uint16_t backgroundcolor)
{
    for (uint8_t i = 0; i < length; ++i) {
        uint8_t digit = (number / lcd_pow(16, length - i - 1)) % 16;
        uint8_t chr = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);

        lcd_show_char(line, column + i, chr,
                      fontcolor, backgroundcolor);
    }
}

void lcd_show_float(uint8_t line, uint8_t column, float number,
                    uint8_t length, uint16_t fontcolor,
                    uint16_t backgroundcolor)
{
    if (length < 3) {
        return;
    }

    uint32_t number1 = (uint32_t)(number * 100.0f);

    for (uint8_t i = 0; i < length; ++i) {
        if (i == length - 2) {
            lcd_show_char(line, column + i, '.',
                          fontcolor, backgroundcolor);
            continue;
        }

        uint8_t digit_pos = (i < length - 2) ? i : i - 1;
        uint32_t divisor = lcd_pow(10, length - 2 - digit_pos);
        uint8_t digit = (number1 / divisor) % 10;

        lcd_show_char(line, column + i, digit + '0',
                      fontcolor, backgroundcolor);
    }
}

void lcd_show_picture(uint8_t *img)
{
    if (img == NULL) {
        return;
    }

    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

    for (size_t offset = 0; offset < LCD_TOTAL_BYTES; offset += LCD_BUF_SIZE) {
        size_t chunk = LCD_TOTAL_BYTES - offset;
        if (chunk > LCD_BUF_SIZE) {
            chunk = LCD_BUF_SIZE;
        }
        lcd_write_datan(img + offset, chunk);
    }
}
