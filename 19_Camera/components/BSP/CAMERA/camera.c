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

/*
 * 第一阶段仅测试 ESP32 -> STM32 串口和左右转向。
 *
 * 不使用 YOLO 结果，不做距离/速度控制。
 * 上电后依次：
 *   STOP 1.5s
 *   LEFT 1.0s
 *   STOP 1.5s
 *   RIGHT 1.0s
 *   STOP 1.5s
 *
 * LEFT  = 左轮 -40，右轮 +40
 * RIGHT = 左轮 +40，右轮 -40
 *
 * 每 100ms 重发一次，确保 STM32 的 700ms 通信超时保护不会触发。
 * 测试完成后保持 STOP，不再自动动作。
 */
static void robot_uart_test(void)
{
    enum {
        TEST_STOP_BEFORE = 0,
        TEST_LEFT,
        TEST_STOP_MIDDLE,
        TEST_RIGHT,
        TEST_STOP_AFTER,
        TEST_DONE
    };

    static int state = TEST_STOP_BEFORE;
    static int64_t state_start_us = 0;
    static int64_t last_send_us = 0;
    static bool initialized = false;

    static const char *state_name[] = {
        "STOP",
        "LEFT",
        "STOP",
        "RIGHT",
        "STOP",
        "DONE"
    };

    const int64_t now_us = esp_timer_get_time();

    if (!initialized) {
        initialized = true;
        state_start_us = now_us;
        last_send_us = 0;
        ESP_LOGI(TAG, "=== ROBOT UART TEST START ===");
        ESP_LOGI(TAG, "STOP 1.5s -> LEFT 1.0s -> STOP 1.5s -> RIGHT 1.0s -> STOP");
    }

    int64_t elapsed_us = now_us - state_start_us;

    switch (state) {
        case TEST_STOP_BEFORE:
            if (elapsed_us >= 1500000) {
                state = TEST_LEFT;
                state_start_us = now_us;
                elapsed_us = 0;
                ESP_LOGI(TAG, "UART TEST: LEFT (-40, +40)");
            }
            break;

        case TEST_LEFT:
            if (elapsed_us >= 1000000) {
                state = TEST_STOP_MIDDLE;
                state_start_us = now_us;
                elapsed_us = 0;
                ESP_LOGI(TAG, "UART TEST: STOP (0, 0)");
            }
            break;

        case TEST_STOP_MIDDLE:
            if (elapsed_us >= 1500000) {
                state = TEST_RIGHT;
                state_start_us = now_us;
                elapsed_us = 0;
                ESP_LOGI(TAG, "UART TEST: RIGHT (+40, -40)");
            }
            break;

        case TEST_RIGHT:
            if (elapsed_us >= 1000000) {
                state = TEST_STOP_AFTER;
                state_start_us = now_us;
                elapsed_us = 0;
                ESP_LOGI(TAG, "UART TEST: STOP (0, 0)");
            }
            break;

        case TEST_STOP_AFTER:
            if (elapsed_us >= 1500000) {
                state = TEST_DONE;
                state_start_us = now_us;
                ESP_LOGI(TAG, "=== ROBOT UART TEST DONE ===");
            }
            break;

        case TEST_DONE:
        default:
            break;
    }

    if (state == TEST_DONE) {
        if (now_us - last_send_us >= 100000) {
            robot_send_speed(0, 0);
            last_send_us = now_us;
        }
        return;
    }

    if (now_us - last_send_us < 100000) {
        return;
    }

    int8_t left = 0;
    int8_t right = 0;

    switch (state) {
        case TEST_LEFT:
            left = -40;
            right = 40;
            break;

        case TEST_RIGHT:
            left = 40;
            right = -40;
            break;

        case TEST_STOP_BEFORE:
        case TEST_STOP_MIDDLE:
        case TEST_STOP_AFTER:
        default:
            left = 0;
            right = 0;
            break;
    }

    robot_send_speed(left, right);
    last_send_us = now_us;
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

    /*
     * 第一阶段串口测试：暂时完全脱离 YOLO，
     * 只验证 ESP32 -> STM32 的左右转指令。
     */
    robot_uart_test();

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
