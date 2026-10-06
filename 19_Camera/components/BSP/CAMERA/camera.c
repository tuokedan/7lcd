#include "camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "lcd.h"

static const char *TAG = "camera";

static camera_config_t camera_config = {
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

    .xclk_freq_hz = 24000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .fb_location = CAMERA_FB_IN_PSRAM,
    .pixel_format = PIXFORMAT_RGB565,
    .frame_size = FRAMESIZE_QVGA,       /* 320x240，与 LCD 完全匹配 */
    .jpeg_quality = 12,
    .fb_count = 2,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

void camera_init(void)
{
    /* RESET 为有效低，PWDN 在硬件上未接 ESP32 GPIO。 */
    CAM_RST(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    CAM_RST(1);
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_err_t ret = esp_camera_init(&camera_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", ret);
        return;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        ESP_LOGI(TAG, "Camera sensor initialized");
        ESP_LOGI(TAG, "Frame size: 320x240, format: RGB565");
    }
}

void camera_show(uint16_t x, uint16_t y)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGW(TAG, "Camera frame capture failed");
        return;
    }

    if (fb->format != PIXFORMAT_RGB565 ||
        x + fb->width > LCD_WIDTH ||
        y + fb->height > LCD_HEIGHT) {
        ESP_LOGW(TAG, "Unsupported frame: %ux%u format=%d",
                 fb->width, fb->height, fb->format);
        esp_camera_fb_return(fb);
        return;
    }

    lcd_set_window(x, y, x + fb->width - 1, y + fb->height - 1);

    /*
     * 摄像头已经直接输出 RGB565，因此无需再复制到 LCD 缓冲区。
     * 直接分块发送 PSRAM 中的帧数据，降低 RAM 占用和 CPU 拷贝开销。
     */
    const size_t chunk_size = 11520;
    size_t offset = 0;

    while (offset < fb->len) {
        size_t chunk = fb->len - offset;
        if (chunk > chunk_size) {
            chunk = chunk_size;
        }

        lcd_write_datan(fb->buf + offset, (uint16_t)chunk);
        offset += chunk;
    }

    esp_camera_fb_return(fb);
}
