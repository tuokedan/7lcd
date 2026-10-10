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
 * UART1 TX = GPIO38 (temporary test choice requested by user)
 * 115200 8N1
 *
 * 当前只使用 TX：
 *   ESP32 GPIO38 -> STM32 PA10 (USART1_RX)
 *   GND          -> GND
 *
 * GPIO38 is selected to avoid GPIO35-37, which may be connected to Octal PSRAM.
 * Note: the board schematic assigns GPIO38 to the SD-card interface (SD_CMD).
 * This UART test assumes the SD-card interface is not active.
 */
#define ROBOT_UART       UART_NUM_1
#define ROBOT_UART_TX    GPIO_NUM_38
#define ROBOT_UART_BAUD  115200

static bool s_robot_uart_ready = false;
static bool s_uart_write_error_logged = false;

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
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Robot UART pin config failed: 0x%x", err);
        return;
    }

    s_robot_uart_ready = true;
    ESP_LOGI(TAG,
             "Robot UART ready: TX GPIO38 -> STM32 PA10, 115200 8N1");
    ESP_LOGW(TAG, "UART TX uses GPIO38; SD-card interface must remain unused during this test");
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

    int written = uart_write_bytes(ROBOT_UART, (const char *)packet, sizeof(packet));
    if (written != (int)sizeof(packet) && !s_uart_write_error_logged) {
        ESP_LOGE(TAG, "UART TX write failed/short: %d of %u bytes",
                 written, (unsigned)sizeof(packet));
        s_uart_write_error_logged = true;
    }
}

/*
 * 人体跟随测试模式：
 * - 每 100ms 发送一帧，持续刷新 STM32 通信看门狗。
 * - 有效检测到人体时以固定基础速度前进，并根据人体框中心差速转向。
 * - 暂时禁用“未检测到人体就停车”的逻辑，未检测到人体时保持直行，
 *   仅用于测试摄像头视野较小时的转向效果。
 * - 不使用人体框高度作为停车条件或速度控制依据。
 */
/*
 * 人体跟随：
 * - 检测到有效人体：固定基础速度 + 根据人体框中心差速转向。
 * - 没有有效检测（包括整屏异常框被过滤）：低速左右交替搜索，
 *   不再一直直行导致摄像头始终看不到目标。
 * - 搜索只用于短时找目标；真实视野仍由镜头视场角决定。
 */
static void robot_follow_task(void *arg)
{
    (void)arg;

    /* 检测框中心 x 落在 150~170 像素内才直行。 */
    const float straight_min_x = 150.0f;
    const float straight_max_x = 170.0f;
    const float confidence_min = 0.45f;
    const int64_t search_switch_us = 800000;
    int64_t search_phase_start_us = esp_timer_get_time();
    int search_direction = 1;
    int64_t last_follow_log_us = 0;

    ESP_LOGW(TAG, "Person-follow: no valid detection triggers slow alternating search");

    while (1) {
        yolo_detection_t detection = {};
        int left = 18;
        int right = 10;
        bool target_valid = false;
        bool searching = false;

        if (yolo_person_get_latest_detection(&detection) &&
            detection.confidence >= confidence_min &&
            detection.class_id == YOLO_PERSON_CLASS) {
            const float center_x = (detection.x1 + detection.x2) * 0.5f;

            target_valid = true;

            const int base_speed = 25;
            int steer = 0;

            /*
             * 中点在 [150,170] 内：直行。
             * 中点 > 170：向一侧转弯；中点 < 150：向另一侧转弯。
             * 转向量按超出边界的像素距离计算。
             */
            if (center_x > straight_max_x) {
                steer = (int)((center_x - straight_max_x) * 0.30f);
            } else if (center_x < straight_min_x) {
                steer = (int)((center_x - straight_min_x) * 0.30f);
            }

            if (steer > 18) steer = 18;
            if (steer < -18) steer = -18;

            left = base_speed + steer;
            right = base_speed - steer;

            if (left > 40) left = 40;
            if (right > 40) right = 40;
            if (left < 0) left = 0;
            if (right < 0) right = 0;

            /* 重新看到人后，下一次丢失从固定搜索方向开始。 */
            search_direction = 1;
            search_phase_start_us = esp_timer_get_time();
        } else {
            const int64_t now = esp_timer_get_time();
            if (now - search_phase_start_us >= search_switch_us) {
                search_direction = -search_direction;
                search_phase_start_us = now;
            }

            /* 低速原地缓转搜索：避免无检测时继续直线驶离目标。 */
            searching = true;
            if (search_direction > 0) {
                left = 18;
                right = 8;
            } else {
                left = 8;
                right = 18;
            }
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_follow_log_us >= 1000000) {
            if (target_valid) {
                ESP_LOGI(TAG,
                         "FOLLOW conf=%.2f center_x=%.0f box=(%.0f,%.0f)-(%.0f,%.0f) L=%d R=%d",
                         detection.confidence,
                         (detection.x1 + detection.x2) * 0.5f,
                         detection.x1, detection.y1,
                         detection.x2, detection.y2,
                         left, right);
            } else if (searching) {
                ESP_LOGW(TAG, "FOLLOW searching slowly; no valid person box L=%d R=%d",
                         left, right);
            }
            last_follow_log_us = now_us;
        }

        robot_send_speed((int8_t)left, (int8_t)right);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void camera_init(void)
{
    CAM_RST(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    CAM_RST(1);
    vTaskDelay(pdMS_TO_TICKS(20));

    /*
     * 先完成摄像头驱动初始化，再启动电机串口测试任务。
     * 避免 UART 测试任务在 esp_camera_init() 的关键初始化阶段抢占 CPU。
     * 串口测试任务使用较低优先级，不应影响摄像头/音视频初始化。
     */
    robot_uart_init();

    esp_err_t ret = esp_camera_init(&camera_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", ret);
        ESP_LOGW(TAG, "Continuing with UART test even though camera init failed");
    } else {
        sensor_t *sensor = esp_camera_sensor_get();
        if (sensor != NULL) {
            ESP_LOGI(TAG, "Camera sensor initialized");
            ESP_LOGI(TAG, "Frame size: 320x240, format: RGB565");
        }
    }

    if (s_robot_uart_ready) {
        BaseType_t task_ok = xTaskCreate(
            robot_follow_task,
            "robot_follow",
            3072,
            NULL,
            2,
            NULL);
        if (task_ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create person-follow task");
        }
    } else {
        ESP_LOGE(TAG, "Robot UART unavailable; person-follow task not started");
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

    /* 提交原始帧给异步 YOLO；推理任务自行复制图像，不阻塞摄像头显示。 */
    (void)yolo_person_submit_frame(fb->buf, fb->width, fb->height);

    yolo_detection_t detection = {};
    if (yolo_person_get_latest_detection(&detection) &&
        detection.confidence >= 0.45f &&
        detection.class_id == YOLO_PERSON_CLASS) {
        draw_detection_box(fb->buf, fb->width, fb->height, &detection);
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
