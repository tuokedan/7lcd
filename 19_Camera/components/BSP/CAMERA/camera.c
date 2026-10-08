#include "camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "lcd.h"
#include "yolo_person.h"
#include "video_stream.h"

#include <stdint.h>

static const char *TAG = "camera";

/*
 * ESP32-S3 -> STM32F103C8T6
 * UART1 TX = GPIO40, RX = GPIO39
 * 115200 8N1
 *
 * 当前只使用 TX：
 *   ESP32 GPIO40 -> STM32 PA10 (USART1_RX)
 *   GND          -> GND
 *
 * GPIO39/40 未被当前摄像头、LCD、音频配置占用。
 */
#define ROBOT_UART       UART_NUM_1
#define ROBOT_UART_TX    GPIO_NUM_40
#define ROBOT_UART_RX    GPIO_NUM_39
#define ROBOT_UART_BAUD  115200

static bool s_robot_uart_ready = false;

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

static void robot_uart_init(void)
{
    if (s_robot_uart_ready) {
        return;
    }

    const uart_config_t config = {
        .baud_rate = ROBOT_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
#if ESP_IDF_VERSION_MAJOR >= 5
        .source_clk = UART_SCLK_DEFAULT,
#endif
    };

    esp_err_t err = uart_driver_install(
        ROBOT_UART, 256, 256, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Robot UART driver install failed: 0x%x", err);
        return;
    }

    err = uart_param_config(ROBOT_UART, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Robot UART config failed: 0x%x", err);
        return;
    }

    err = uart_set_pin(
        ROBOT_UART,
        ROBOT_UART_TX,
        ROBOT_UART_RX,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Robot UART pin config failed: 0x%x", err);
        return;
    }

    s_robot_uart_ready = true;
    ESP_LOGI(TAG,
             "Robot UART ready: TX GPIO40 -> STM32 PA10, 115200 8N1");
}

/*
 * LEFT/RIGHT 为 -100~100。
 * 正数前进，负数后退。
 * 协议：AA 55 LEFT RIGHT CHECK
 */
static void robot_send_speed(int8_t left, int8_t right)
{
    if (!s_robot_uart_ready) {
        return;
    }

    uint8_t packet[5];
    packet[0] = 0xAA;
    packet[1] = 0x55;
    packet[2] = (uint8_t)left;
    packet[3] = (uint8_t)right;
    packet[4] = (uint8_t)(packet[0] ^ packet[1] ^ packet[2] ^ packet[3]);

    (void)uart_write_bytes(ROBOT_UART, packet, sizeof(packet));
}

static int clamp_speed(int value)
{
    if (value > 100) return 100;
    if (value < -100) return -100;
    return value;
}

/*
 * 根据人物框中心控制转向，根据人物框高度粗略估计距离。
 *
 * 320x240 图像：
 *   cx < 135       -> 向左
 *   135~185        -> 基本直行
 *   cx > 185       -> 向右
 *
 * 人物框越高，说明人越近，车速越低；
 * 高度 >= 170 像素时停车，防止跟得过近。
 */
static void robot_follow_person(const yolo_detection_t *detection)
{
    if (detection == NULL ||
        detection->confidence < 0.35f) {
        robot_send_speed(0, 0);
        return;
    }

    const float cx = (detection->x1 + detection->x2) * 0.5f;
    const float box_height = detection->y2 - detection->y1;

    if (box_height >= 170.0f) {
        robot_send_speed(0, 0);
        return;
    }

    int base_speed;

    if (box_height < 55.0f) {
        base_speed = 70;
    } else if (box_height < 85.0f) {
        base_speed = 55;
    } else if (box_height < 115.0f) {
        base_speed = 40;
    } else {
        base_speed = 22;
    }

    float error = cx - 160.0f;

    if (error > -20.0f && error < 20.0f) {
        error = 0.0f;
    }

    /* 比例转向，右侧目标 -> 左轮加速、右轮减速。 */
    int turn = (int)(error * 0.35f);

    if (turn > 45) turn = 45;
    if (turn < -45) turn = -45;

    /*
     * 人偏得很厉害时降低前进速度，让车辆优先把车头转向人。
     */
    if (error > 100.0f || error < -100.0f) {
        if (base_speed > 25) {
            base_speed = 25;
        }
    }

    int left = clamp_speed(base_speed + turn);
    int right = clamp_speed(base_speed - turn);

    robot_send_speed((int8_t)left, (int8_t)right);
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

    robot_uart_init();

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
        robot_send_speed(0, 0);
        return;
    }

    if (fb->format != PIXFORMAT_RGB565 ||
        x + fb->width > LCD_WIDTH ||
        y + fb->height > LCD_HEIGHT) {
        ESP_LOGW(TAG, "Unsupported frame: %ux%u format=%d",
                 fb->width, fb->height, fb->format);
        robot_send_speed(0, 0);
        esp_camera_fb_return(fb);
        return;
    }

    (void)yolo_person_submit_frame(fb->buf, fb->width, fb->height);

    yolo_detection_t detection;
    bool have_detection =
        yolo_person_get_latest_detection(&detection) &&
        detection.confidence >= 0.35f;

    if (have_detection) {
        draw_detection_box(fb->buf, fb->width, fb->height, &detection);
        robot_follow_person(&detection);
    } else {
        /* 没检测到人，立即停车。 */
        robot_send_speed(0, 0);
    }

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
