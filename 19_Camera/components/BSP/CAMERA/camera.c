#include "camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "lcd.h"
#include "yolo_person.h"
#include "video_stream.h"

#include <stdint.h>

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
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 12,
    .fb_count = 3,
    .grab_mode = CAMERA_GRAB_LATEST,
};

static inline void draw_pixel_rgb565(uint8_t *buf,
                                     uint16_t width,
                                     uint16_t height,
                                     int x,
                                     int y,
                                     uint16_t color)
{
    if (buf == NULL || x < 0 || y < 0 ||
        x >= width || y >= height) {
        return;
    }

    size_t offset = ((size_t)y * width + (size_t)x) * 2;
    buf[offset] = (uint8_t)(color >> 8);
    buf[offset + 1] = (uint8_t)(color & 0xFF);
}

static void draw_detection_box(uint8_t *buf,
                               uint16_t width,
                               uint16_t height,
                               const yolo_detection_t *detection)
{
    if (buf == NULL || detection == NULL) {
        return;
    }

    int x1 = (int)detection->x1;
    int y1 = (int)detection->y1;
    int x2 = (int)detection->x2;
    int y2 = (int)detection->y2;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= width) x2 = width - 1;
    if (y2 >= height) y2 = height - 1;

    if (x2 <= x1 || y2 <= y1) {
        return;
    }

    const uint16_t color = RED;

    for (int x = x1; x <= x2; ++x) {
        draw_pixel_rgb565(buf, width, height, x, y1, color);
        draw_pixel_rgb565(buf, width, height, x, y1 + 1, color);
        draw_pixel_rgb565(buf, width, height, x, y2, color);
        draw_pixel_rgb565(buf, width, height, x, y2 - 1, color);
    }

    for (int y = y1; y <= y2; ++y) {
        draw_pixel_rgb565(buf, width, height, x1, y, color);
        draw_pixel_rgb565(buf, width, height, x1 + 1, y, color);
        draw_pixel_rgb565(buf, width, height, x2, y, color);
        draw_pixel_rgb565(buf, width, height, x2 - 1, y, color);
    }
}

void camera_init(void)
{
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

    /* Run YOLO person detection on the captured frame. */
    (void)yolo_person_submit_frame(fb->buf, fb->width, fb->height);

    yolo_detection_t detection;
    if (yolo_person_get_latest_detection(&detection) &&
        detection.confidence >= 0.35f) {
        draw_detection_box(fb->buf, fb->width, fb->height, &detection);
    }

    /*
     * PC 回传视频运行时，video_stream.c 会直接把 PC 画面刷到 LCD。
     * 此时这里不能再刷摄像头，否则两个任务会交替改写同一块 LCD，导致画面闪烁/错乱。
     * PC 回传停止约 1.5 秒后自动恢复本机摄像头显示。
     */
    if (!video_stream_pc_video_active()) {
        lcd_lock();

        lcd_set_window(x, y, x + fb->width - 1, y + fb->height - 1);

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

        lcd_unlock();
    }

    video_stream_publish_frame(fb);

    esp_camera_fb_return(fb);
}
