#include "camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "driver/gpio.h"
#include "lcd.h"

extern uint8_t lcd_buf[115200];

/* 摄像头配置 */
static camera_config_t camera_config = {
    /* 引脚配置 */
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,
    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,
    /* 图像配置 */
    .xclk_freq_hz = 24000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .pixel_format = PIXFORMAT_RGB565,       /* 图像输出模式 */
    .frame_size = FRAMESIZE_240X240,        /* 图像输出大小 */
    .jpeg_quality = 5,                      /* 0-63，对于OV系列相机传感器，数量越少意味着质量越高 */
    .fb_count = 2,                          /* 当使用jpeg模式时，如果fb_count超过一个，则驱动程序将在连续模式下工作 */
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

/**
 * @brief       摄像头初始化
 * @param       cmd 传输的8位命令数据
 * @retval      无
 */
void camera_init(void)
{
    CAM_PWDN(0);
    CAM_RST(0);
    vTaskDelay(20);
    CAM_RST(1);
    vTaskDelay(20);

    esp_camera_init(&camera_config);
}

unsigned long i = 0;
unsigned long j = 0;
camera_fb_t *fb = NULL;

/**
 * @brief       显示摄像头数据（RGB565）
 * @param       x：x轴坐标
 * @param       y：y轴坐标
 * @retval      无
 */
void camera_show(uint16_t x, uint16_t y)
{
    fb = esp_camera_fb_get();

    lcd_set_window(x, y, x + fb->width - 1, y + fb->height - 1);
 
    for (j = 0; j < fb->width * fb->height; j++)                /* lcd_buf存储摄像头整一帧RGB数据 */
    {
        lcd_buf[2 * j] = (fb->buf[2 * i]) ;
        lcd_buf[2 * j + 1] =  (fb->buf[2 * i + 1]);
        i ++;
    }
    
    for(j = 0; j < (fb->width * fb->height * 2 / 11520); j++)   /* 例如：240*240*2/11520 = 10;分10次发送RGB数据,即：将LCD十等分发送图片数据 */
    {  
        lcd_write_datan(&lcd_buf[j * 11520] , 11520);           /* &lcd_buf[j * LCD_BUF_SIZE] 偏移地址发送数据 */
    }
    esp_camera_fb_return(fb);
    i = 0;
    fb = NULL;
}
