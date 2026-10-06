# ESP32-S3 V1.4 LCD Example

基于 ESP-IDF 的 ST7789 LCD 测试工程。

## LCD 引脚

根据仓库中的《ESP32-S3 V1.4原理图.pdf》：

| LCD 信号 | ESP32-S3 GPIO |
|---|---:|
| LCD_CLK | GPIO42 |
| LCD_CS1 | GPIO45 |
| LCD_RESET | GPIO2 |
| LCD_DC | GPIO41 |
| LCD_MOSI | GPIO48 |
| LCD_MISO | GPIO47 |

本工程使用 SPI2。

> 注意：GPIO42 是 LCD_CLK，不是背光控制脚。原示例把 GPIO42 同时作为 BLK 使用，这是错误的，现已删除背光 GPIO 控制。

## 当前测试

启动后清屏并显示：

- A
- HELLOWORLD
- 十进制数字
- 十六进制数字
- 浮点数

后续可以在此工程基础上继续加入 OV2640 摄像头采集和图像显示。
