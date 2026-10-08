#include "video_stream.h"
#include "lcd.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "img_converters.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "video";

#define VIDEO_AP_SSID       "ESP32-CAM-Video"
#define VIDEO_AP_PASSWORD   "12345678"
#define VIDEO_HTTP_PORT     80

#define VIDEO_JPEG_QUALITY  55
#define VIDEO_MAX_JPEG_SIZE (128 * 1024)
#define VIDEO_BUFFER_COUNT  3
#define VIDEO_FRAME_INTERVAL_US 200000 /* about 5 FPS */

#define PC_VIDEO_MAX_JPEG_SIZE (128 * 1024)
#define PC_VIDEO_RGB_SIZE  (320 * 240 * 2)
#define PC_VIDEO_RGB888_SIZE (320 * 240 * 3)
static uint8_t *s_pc_jpeg = NULL;
static uint8_t *s_pc_rgb888 = NULL;
static uint8_t *s_pc_rgb565 = NULL;

typedef struct {
    uint8_t *data;
    size_t len;
    volatile int readers;
} video_buffer_t;

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t written;
    bool overflow;
} jpeg_writer_t;

static video_buffer_t s_buffers[VIDEO_BUFFER_COUNT] = {};
static int s_current_buffer = -1;
static uint32_t s_frame_seq = 0;
static SemaphoreHandle_t s_buffer_mutex = NULL;
static bool s_started = false;
static int64_t s_last_encode_us = 0;
static volatile int64_t s_last_pc_video_us = 0;
#define PC_VIDEO_ACTIVE_TIMEOUT_US 1500000

static size_t jpeg_write_cb(void *arg, size_t index, const void *data, size_t len)
{
    jpeg_writer_t *writer = (jpeg_writer_t *)arg;

    (void)index;

    if (writer == NULL || data == NULL) {
        return 0;
    }

    if (writer->written + len > writer->capacity) {
        writer->overflow = true;
        return 0;
    }

    memcpy(writer->buffer + writer->written, data, len);
    writer->written += len;
    return len;
}

static int find_write_buffer(void)
{
    int selected = -1;

    for (int i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        if (i == s_current_buffer) {
            continue;
        }

        if (s_buffers[i].readers == 0) {
            selected = i;
            break;
        }
    }

    return selected;
}

static esp_err_t video_root_handler(httpd_req_t *req)
{
    const char *html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<title>ESP32 Camera</title></head><body>"
        "<h3>ESP32-S3 Camera Stream</h3>"
        "<img src='/video' width='320' height='240'>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t pc_video_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > PC_VIDEO_MAX_JPEG_SIZE) {
        ESP_LOGW(TAG, "PC JPEG size invalid: %u", (unsigned)req->content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JPEG too large or empty");
        return ESP_FAIL;
    }

    if (s_pc_jpeg == NULL || s_pc_rgb888 == NULL || s_pc_rgb565 == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Video buffers unavailable");
        return ESP_FAIL;
    }

    size_t total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(
            req,
            (char *)s_pc_jpeg + total,
            req->content_len - total);

        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGW(TAG, "PC JPEG receive failed: %d", received);
            return ESP_FAIL;
        }

        total += (size_t)received;
    }

    /*
     * 不再直接使用 jpg2rgb565()：
     *
     * 不同版本的 esp32-camera / JPEG decoder 对 RGB565 输出字节序
     * 存在差异。直接交换 RGB565 两个字节虽然能消除部分花屏，
     * 但容易留下颜色失真。
     *
     * 这里先解码为标准 RGB888，再由我们明确打包成 RGB565 BE。
     * RGB888 的三个通道分别占一个字节，因此不会再产生字节序歧义。
     */
    if (!fmt2rgb888(
            s_pc_jpeg,
            total,
            PIXFORMAT_JPEG,
            s_pc_rgb888)) {
        ESP_LOGW(TAG, "PC JPEG -> RGB888 decode failed, size=%u",
                 (unsigned)total);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JPEG");
        return ESP_FAIL;
    }

    for (size_t pixel = 0; pixel < 320 * 240; ++pixel) {
        const uint8_t r = s_pc_rgb888[pixel * 3 + 0];
        const uint8_t g = s_pc_rgb888[pixel * 3 + 1];
        const uint8_t b = s_pc_rgb888[pixel * 3 + 2];

        /*
         * RGB888 -> RGB565
         * ST7789 使用 16-bit RGB565，数据高字节在前。
         */
        const uint16_t rgb565 =
            (static_cast<uint16_t>(r & 0xF8) << 8) |
            (static_cast<uint16_t>(g & 0xFC) << 3) |
            (static_cast<uint16_t>(b) >> 3);

        s_pc_rgb565[pixel * 2 + 0] = static_cast<uint8_t>(rgb565 >> 8);
        s_pc_rgb565[pixel * 2 + 1] = static_cast<uint8_t>(rgb565 & 0xFF);
    }

    /* 收到 PC 画面即认为反向视频处于活动状态；camera_show() 会暂停本机摄像头刷屏。 */
    s_last_pc_video_us = esp_timer_get_time();

    /* PC 摄像头固定发送 320x240，因此直接整屏显示。 */
    lcd_lock();
    lcd_show_picture(s_pc_rgb565);
    lcd_unlock();

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t video_stream_handler(httpd_req_t *req)
{
    static const char *content_type =
        "multipart/x-mixed-replace; boundary=frame";

    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    uint32_t last_seq = 0;

    ESP_LOGI(TAG, "PC video client connected");

    while (true) {
        int buffer_index = -1;

        for (int retry = 0; retry < 100; ++retry) {
            if (xSemaphoreTake(s_buffer_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
                continue;
            }

            if (s_current_buffer >= 0 && s_frame_seq != last_seq) {
                buffer_index = s_current_buffer;
                s_buffers[buffer_index].readers++;
                last_seq = s_frame_seq;
            }

            xSemaphoreGive(s_buffer_mutex);

            if (buffer_index >= 0) {
                break;
            }
        }

        if (buffer_index < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        video_buffer_t *buffer = &s_buffers[buffer_index];

        char header[96];
        int header_len = snprintf(
            header, sizeof(header),
            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n\r\n",
            (unsigned)buffer->len);

        esp_err_t ret = httpd_resp_send_chunk(req, header, header_len);
        if (ret == ESP_OK) {
            ret = httpd_resp_send_chunk(
                req, (const char *)buffer->data, buffer->len);
        }
        if (ret == ESP_OK) {
            ret = httpd_resp_send_chunk(req, "\r\n", 2);
        }

        if (xSemaphoreTake(s_buffer_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_buffers[buffer_index].readers--;
            xSemaphoreGive(s_buffer_mutex);
        }

        if (ret != ESP_OK) {
            ESP_LOGI(TAG, "PC video client disconnected");
            break;
        }
    }

    return ESP_OK;
}

static bool start_wifi_ap(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: 0x%x", ret);
        return false;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: 0x%x", ret);
        return false;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: 0x%x", ret);
        return false;
    }

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi AP netif");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: 0x%x", ret);
        return false;
    }

    wifi_config_t wifi_config = {};
    memcpy(wifi_config.ap.ssid, VIDEO_AP_SSID, sizeof(VIDEO_AP_SSID) - 1);
    memcpy(wifi_config.ap.password, VIDEO_AP_PASSWORD, sizeof(VIDEO_AP_PASSWORD) - 1);
    wifi_config.ap.ssid_len = sizeof(VIDEO_AP_SSID) - 1;
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 2;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = false;

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: 0x%x", ret);
        return false;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: 0x%x", ret);
        return false;
    }

    ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) failed: 0x%x", ret);
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start failed: 0x%x", ret);
        return false;
    }

    ESP_LOGI(TAG, "Wi-Fi AP started");
    ESP_LOGI(TAG, "SSID: %s", VIDEO_AP_SSID);
    ESP_LOGI(TAG, "Password: %s", VIDEO_AP_PASSWORD);
    ESP_LOGI(TAG, "AP IP: 192.168.4.1");
    return true;
}

static void start_http_server(void)
{
    /*
     * /video is a long-lived MJPEG connection.  ESP-IDF HTTP server
     * executes the URI handler in its server task, so keeping /video and
     * /pcvideo on the same server can block the POST handler and cause
     * PC -> ESP32 timeouts.  Use a second HTTP server on port 81 for
     * reverse video.
     */
    httpd_config_t video_config = HTTPD_DEFAULT_CONFIG();
    video_config.server_port = VIDEO_HTTP_PORT;
    video_config.max_uri_handlers = 2;
    video_config.stack_size = 8192;

    httpd_handle_t video_server = NULL;
    esp_err_t ret = httpd_start(&video_server, &video_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Video HTTP server start failed: 0x%x", ret);
        return;
    }

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = video_root_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t video_uri = {
        .uri = "/video",
        .method = HTTP_GET,
        .handler = video_stream_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(video_server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(video_server, &video_uri));

    httpd_config_t pc_config = HTTPD_DEFAULT_CONFIG();
    pc_config.server_port = VIDEO_HTTP_PORT + 1;
    /* Each httpd instance also needs its own internal control socket port. */
    pc_config.ctrl_port = 32769;
    pc_config.max_uri_handlers = 1;
    pc_config.stack_size = 8192;

    httpd_handle_t pc_server = NULL;
    ret = httpd_start(&pc_server, &pc_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PC video HTTP server start failed: 0x%x", ret);
        return;
    }

    httpd_uri_t pc_video_uri = {
        .uri = "/pcvideo",
        .method = HTTP_POST,
        .handler = pc_video_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(pc_server, &pc_video_uri));

    ESP_LOGI(TAG, "HTTP video server started: http://192.168.4.1:%d/video",
             VIDEO_HTTP_PORT);
    ESP_LOGI(TAG, "HTTP PC video server started: http://192.168.4.1:%d/pcvideo",
             VIDEO_HTTP_PORT + 1);
}

void video_stream_init(void)
{
    if (s_started) {
        return;
    }

    s_buffer_mutex = xSemaphoreCreateMutex();
    if (s_buffer_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create video mutex");
        return;
    }

    for (int i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        s_buffers[i].data = heap_caps_malloc(
            VIDEO_MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (s_buffers[i].data == NULL) {
            ESP_LOGE(TAG, "Failed to allocate JPEG buffer %d", i);
            for (int j = 0; j < i; ++j) {
                heap_caps_free(s_buffers[j].data);
                s_buffers[j].data = NULL;
            }
            vSemaphoreDelete(s_buffer_mutex);
            s_buffer_mutex = NULL;
            return;
        }
    }

    s_pc_jpeg = heap_caps_malloc(
        PC_VIDEO_MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pc_rgb888 = heap_caps_malloc(
        PC_VIDEO_RGB888_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pc_rgb565 = heap_caps_malloc(
        PC_VIDEO_RGB_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_pc_jpeg == NULL || s_pc_rgb888 == NULL || s_pc_rgb565 == NULL) {
        ESP_LOGE(TAG, "PC reverse-video buffers allocation failed");

        if (s_pc_jpeg) {
            heap_caps_free(s_pc_jpeg);
        }
        if (s_pc_rgb888) {
            heap_caps_free(s_pc_rgb888);
        }
        if (s_pc_rgb565) {
            heap_caps_free(s_pc_rgb565);
        }

        s_pc_jpeg = NULL;
        s_pc_rgb888 = NULL;
        s_pc_rgb565 = NULL;
        return;
    }

    if (!start_wifi_ap()) {
        ESP_LOGE(TAG, "Wi-Fi AP startup failed; video HTTP servers will not start");
        return;
    }

    start_http_server();

    s_started = true;
}

void video_stream_publish_frame(const camera_fb_t *fb)
{
    if (!s_started || fb == NULL || fb->format != PIXFORMAT_RGB565) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (s_last_encode_us != 0 &&
        now_us - s_last_encode_us < VIDEO_FRAME_INTERVAL_US) {
        return;
    }

    int write_index = -1;

    if (xSemaphoreTake(s_buffer_mutex, 0) != pdTRUE) {
        return;
    }

    write_index = find_write_buffer();
    xSemaphoreGive(s_buffer_mutex);

    if (write_index < 0) {
        return;
    }

    jpeg_writer_t writer = {
        .buffer = s_buffers[write_index].data,
        .capacity = VIDEO_MAX_JPEG_SIZE,
        .written = 0,
        .overflow = false,
    };

    if (!frame2jpg_cb(
            (camera_fb_t *)fb,
            VIDEO_JPEG_QUALITY,
            jpeg_write_cb,
            &writer) ||
        writer.overflow ||
        writer.written == 0) {
        ESP_LOGW(TAG, "JPEG encode failed or output too large");
        return;
    }

    if (xSemaphoreTake(s_buffer_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }

    /*
     * The buffer was not current while encoding. It can only become
     * current here, so a reader cannot be using it yet.
     */
    s_buffers[write_index].len = writer.written;
    s_buffers[write_index].readers = 0;
    s_current_buffer = write_index;
    s_frame_seq++;
    s_last_encode_us = now_us;

    xSemaphoreGive(s_buffer_mutex);
}


bool video_stream_pc_video_active(void)
{
    int64_t last_us = s_last_pc_video_us;
    if (last_us == 0) {
        return false;
    }

    return (esp_timer_get_time() - last_us) < PC_VIDEO_ACTIVE_TIMEOUT_US;
}
