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
#define PC_VIDEO_RGB_SIZE (320 * 240 * 2)
static uint8_t *s_pc_jpeg = NULL;
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

    if (s_pc_jpeg == NULL || s_pc_rgb565 == NULL) {
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

    if (!jpg2rgb565(
            s_pc_jpeg,
            total,
            s_pc_rgb565,
            JPG_SCALE_NONE)) {
        ESP_LOGW(TAG, "PC JPEG decode failed, size=%u", (unsigned)total);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JPEG");
        return ESP_FAIL;
    }

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

    s_pc_jpeg = heap_caps_malloc(PC_VIDEO_MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pc_rgb565 = heap_caps_malloc(PC_VIDEO_RGB_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pc_jpeg == NULL || s_pc_rgb565 == NULL) {
        ESP_LOGE(TAG, "PC reverse-video buffers allocation failed");
        if (s_pc_jpeg) heap_caps_free(s_pc_jpeg);
        if (s_pc_rgb565) heap_caps_free(s_pc_rgb565);
        s_pc_jpeg = NULL;
        s_pc_rgb565 = NULL;
        return;
    }

    start_wifi_ap();
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
