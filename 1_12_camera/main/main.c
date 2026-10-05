/* Camera Example

    This example code is in the Public Domain (or CC0 licensed, at your option.)
    Unless required by applicable law or agreed to in writing, this
    software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
    CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "lcd.h"
#include "zk.h"
#include "jpegd2.h"

static const char *TAG = "main";

//摄像头图像帧大小
#define CAMERA_FRAMESIZE FRAMESIZE_QVGA


//缩放显存
//buff:存储缩放后点阵信息缓冲，w1和h1表示缓冲的宽和高
//bitmap:需要缩放的点阵信息，w2和h2表示点阵的宽和高
//w和h，表示缩放后点阵的宽和高
void zoom_bitmap(uint16_t* w, uint16_t* h, uint16_t w1, uint16_t h1, uint16_t *buff, uint16_t w2, uint16_t h2, uint16_t *bitmap)
{
    if(w1>=w2 && h1>=h2)
    {
        memcpy((void*)buff, (void*)bitmap, w2*h2*2);
        *w=w2;
        *h=h2;
    }
    else
    {
        double w_zoom=(double)w2/(double)w1;
        double h_zoom=(double)h2/(double)h1;
        uint32_t offset=0;
        uint32_t yy1=0,yy2=0;


//        printf("w_zoom=%f,h_zoom=%f.\r\n", w_zoom,h_zoom);

        if(w_zoom-h_zoom>=0)
        {
            //以宽缩放为准
            uint16_t actual_w=w1;
            uint16_t actual_h=(uint16_t)((double)h2/w_zoom);


            for(int y=0; y<actual_h; y++)
            {
                yy1=y*actual_w;
                yy2=(uint16_t)(y*w_zoom);
                for(int x=0; x<actual_w; x++)
                {
                    uint16_t xx=(uint16_t)(x*w_zoom);
                    *(buff+yy1+x)=*(bitmap+yy2*w2+xx);
                }
            }

            *w=actual_w;
            *h=actual_h;
        }
        else
        {
           printf("++++++++++++++++++++22222.\r\n");

           //以高缩放为准
           uint16_t actual_w=(uint16_t)((double)w2/h_zoom);
           uint16_t actual_h=h2;

           for(int y=0; y<actual_h; y++)
           {
               yy1=y*actual_w;
               yy2=(uint16_t)(y*h_zoom);
               for(int x=0; x<actual_w; x++)
               {
                   uint16_t xx=(uint16_t)(x*h_zoom);
                   *(buff+yy1+x)=*(bitmap+yy2*w2+xx);
               }
           }

           *w=actual_w;
           *h=actual_h;
        }
    }
}



void app_main()
{
    uint16_t buff_width, buff_height;

    lcd_init();

    camera_config_t camera_config = {
        .pin_pwdn = -1,
        .pin_reset = GPIO_NUM_15,
        .pin_xclk = -1,
        .pin_sscb_sda = GPIO_NUM_6,
        .pin_sscb_scl = GPIO_NUM_4,

        .pin_d7 = GPIO_NUM_10,
        .pin_d6 = GPIO_NUM_9,
        .pin_d5 = GPIO_NUM_46,
        .pin_d4 = GPIO_NUM_3,
        .pin_d3 = GPIO_NUM_8,
        .pin_d2 = GPIO_NUM_18,
        .pin_d1 = GPIO_NUM_17,
        .pin_d0 = GPIO_NUM_16,
        .pin_vsync = GPIO_NUM_5,
        .pin_href = GPIO_NUM_7,
        .pin_pclk = GPIO_NUM_11,

        //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
#ifdef CONFIG_CAMERA_JPEG_MODE
        .pixel_format = PIXFORMAT_JPEG, //YUV422,GRAYSCALE,RGB565,JPEG
#else
        .pixel_format = PIXFORMAT_RGB565,
#endif
        .frame_size = CAMERA_FRAMESIZE,    //QQVGA-UXGA Do not use sizes above QVGA when not JPEG
        .jpeg_quality = 12, //0-63 lower number means higher quality
        .fb_count = 2       //if more than one, i2s runs in continuous mode. Use only with JPEG
    };
    //initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed");
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    s->set_vflip(s, 1);//flip it back
    //initial sensors are flipped vertically and colors are a bit saturated
    if (s->id.PID == OV3660_PID) {
        s->set_brightness(s, 1);//up the blightness just a bit
        s->set_saturation(s, -2);//lower the saturation
    }
    //drop down frame size for higher initial frame rate
    s->set_framesize(s, CAMERA_FRAMESIZE);
    ESP_LOGI(TAG, "Camera Init done");

#ifdef CONFIG_CAMERA_JPEG_MODE
    ESP_LOGI(TAG, "Camera jpeg mode");
    uint8_t *rgb565 = NULL;
    if(CAMERA_FRAMESIZE<=FRAMESIZE_QVGA)
    {
      rgb565 = malloc(240 * 320 * 2);
    }
    else if(CAMERA_FRAMESIZE<=FRAMESIZE_XGA)
    {
      rgb565 = malloc(1024 * 768 * 2);
    }
    else if(CAMERA_FRAMESIZE<=FRAMESIZE_UXGA)
    {
      rgb565 = malloc(1600 * 1200 * 2);
    }
    else if(CAMERA_FRAMESIZE<=FRAMESIZE_QXGA)
    {
      rgb565 = malloc(2048*1536 * 2);
    }
    else
    {
      ESP_LOGE(TAG, "camera frame size error!!");
    }

    if (NULL == rgb565) {
        ESP_LOGE(TAG, "can't alloc memory for rgb565 buffer");
        return;
    }
#endif

    while (1) {
        camera_fb_t *pic = esp_camera_fb_get();//等待一帧图像信息
        if (pic) {
            ESP_LOGI(TAG, "picture: %d x %d %dbyte", pic->width, pic->height, pic->len);
#ifdef CONFIG_CAMERA_JPEG_MODE
            //摄像头输出的是JPG格式，需要解码
            //JPG转RGB565
            mjpegdraw(pic->buf, pic->len, (uint8_t *)rgb565, NULL);
            zoom_bitmap(&buff_width, &buff_height, lcd_GetWidth(), lcd_GetHeight(), lcd_GetBuff(), pic->width, pic->height, rgb565);

            Gui_DrawFont_GBK24(0,100,RED,WHITE, 1, (u8*)"深圳市亿研电子有限公司");
            lcd_update();//刷新显示
#else
            //摄像头输出的是BMP格式，可以直接显示到显示屏
            zoom_bitmap(&buff_width, &buff_height, lcd_GetWidth(), lcd_GetHeight(), lcd_GetBuff(), pic->width, pic->height, pic->buf);
            lcd_update();//刷新显示
#endif
            esp_camera_fb_return(pic);
        } else {
            ESP_LOGE(TAG, "Get frame failed");
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

#ifdef CONFIG_CAMERA_JPEG_MODE
    free(rgb565);
#endif
}
