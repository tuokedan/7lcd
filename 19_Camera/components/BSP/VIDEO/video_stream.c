#include "video_stream.h"

#include <string.h>

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
            if (httpd_req_to_sockfd(req) < 0) {
                break;
            }
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
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = VIDEO_HTTP_PORT;
    config.max_uri_handlers = 4;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: 0x%x", ret);
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

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &video_uri));

    ESP_LOGI(TAG, "HTTP video server started: http://192.168.4.1/video");
}

static void start_wifi_ap(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = VIDEO_AP_SSID,
            .ssid_len = sizeof(VIDEO_AP_SSID) - 1,
            .channel = 6,
            .password = VIDEO_AP_PASSWORD,
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started");
    ESP_LOGI(TAG, "SSID: %s", VIDEO_AP_SSID);
    ESP_LOGI(TAG, "Password: %s", VIDEO_AP_PASSWORD);
    ESP_LOGI(TAG, "Connect PC to the AP, then open 192.168.4.1");
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
