#include "yolo_person.h"

#include "pedestrian_detect.hpp"
#include "dl_image_define.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <list>
#include <cstring>

static const char *TAG = "yolo";

static constexpr uint16_t FRAME_WIDTH = YOLO_CAMERA_WIDTH;
static constexpr uint16_t FRAME_HEIGHT = YOLO_CAMERA_HEIGHT;
static constexpr size_t FRAME_BYTES =
    static_cast<size_t>(FRAME_WIDTH) * FRAME_HEIGHT * 2;
static constexpr int BUFFER_COUNT = 2;

static PedestrianDetect *s_detector = nullptr;
static uint8_t *s_frame_buffers[BUFFER_COUNT] = {nullptr, nullptr};

/* 0=空闲，1=已排队，2=正在推理。 */
static volatile uint8_t s_buffer_state[BUFFER_COUNT] = {0, 0};
static QueueHandle_t s_frame_queue = nullptr;

static portMUX_TYPE s_result_mux = portMUX_INITIALIZER_UNLOCKED;
static yolo_detection_t s_latest_detection = {};
static bool s_latest_valid = false;

static bool find_free_buffer(int *index)
{
    bool found = false;

    portENTER_CRITICAL(&s_result_mux);
    for (int i = 0; i < BUFFER_COUNT; ++i) {
        if (s_buffer_state[i] == 0) {
            s_buffer_state[i] = 1;
            *index = i;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_result_mux);

    return found;
}

static void release_buffer(int index)
{
    portENTER_CRITICAL(&s_result_mux);
    s_buffer_state[index] = 0;
    portEXIT_CRITICAL(&s_result_mux);
}

static void update_latest_detection(const yolo_detection_t *detection,
                                    bool valid)
{
    portENTER_CRITICAL(&s_result_mux);
    if (valid && detection != nullptr) {
        s_latest_detection = *detection;
    }
    s_latest_valid = valid;
    portEXIT_CRITICAL(&s_result_mux);
}

static void yolo_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Pedestrian detection task started on CPU%d",
             xPortGetCoreID());

    while (true) {
        uint8_t index = 0;

        if (xQueueReceive(s_frame_queue, &index, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /*
         * 如果队列里已经有更新的帧，只保留最新帧，避免检测结果滞后。
         */
        uint8_t newer_index = 0;
        while (xQueueReceive(s_frame_queue, &newer_index, 0) == pdTRUE) {
            release_buffer(index);
            index = newer_index;
        }

        portENTER_CRITICAL(&s_result_mux);
        s_buffer_state[index] = 2;
        portEXIT_CRITICAL(&s_result_mux);

        yolo_detection_t detection = {};
        int64_t infer_start = esp_timer_get_time();

        bool found_person = yolo_person_detect_rgb565(
            s_frame_buffers[index],
            FRAME_WIDTH,
            FRAME_HEIGHT,
            &detection);

        int64_t infer_ms = (esp_timer_get_time() - infer_start) / 1000;

        if (found_person) {
            update_latest_detection(&detection, true);

            ESP_LOGI(TAG,
                     "person conf=%.2f bbox=(%.0f,%.0f)-(%.0f,%.0f), infer=%lld ms",
                     detection.confidence,
                     detection.x1, detection.y1,
                     detection.x2, detection.y2,
                     (long long)infer_ms);
        } else {
            update_latest_detection(nullptr, false);
            ESP_LOGI(TAG, "no person, infer=%lld ms",
                     (long long)infer_ms);
        }

        release_buffer(index);

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

extern "C" void yolo_person_init(void)
{
    if (s_detector != nullptr) {
        return;
    }

    ESP_LOGI(TAG,
             "Initializing Espressif Pedestrian Detect PICO_S8_V1...");

    s_detector = new PedestrianDetect(PedestrianDetect::PICO_S8_V1);

    if (s_detector == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate PedestrianDetect");
        return;
    }

    for (int i = 0; i < BUFFER_COUNT; ++i) {
        s_frame_buffers[i] = static_cast<uint8_t *>(
            heap_caps_malloc(FRAME_BYTES,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

        if (s_frame_buffers[i] == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate frame buffer %d", i);

            for (int j = 0; j < i; ++j) {
                heap_caps_free(s_frame_buffers[j]);
                s_frame_buffers[j] = nullptr;
            }

            delete s_detector;
            s_detector = nullptr;
            return;
        }
    }

    s_frame_queue = xQueueCreate(BUFFER_COUNT, sizeof(uint8_t));
    if (s_frame_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create frame queue");

        for (int i = 0; i < BUFFER_COUNT; ++i) {
            heap_caps_free(s_frame_buffers[i]);
            s_frame_buffers[i] = nullptr;
        }

        delete s_detector;
        s_detector = nullptr;
        return;
    }

    BaseType_t task_ok = xTaskCreatePinnedToCore(
        yolo_task,
        "ped_detect",
        8192,
        nullptr,
        5,
        nullptr,
        1);

    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create pedestrian detection task");

        vQueueDelete(s_frame_queue);
        s_frame_queue = nullptr;

        for (int i = 0; i < BUFFER_COUNT; ++i) {
            heap_caps_free(s_frame_buffers[i]);
            s_frame_buffers[i] = nullptr;
        }

        delete s_detector;
        s_detector = nullptr;
        return;
    }

    ESP_LOGI(TAG, "Pedestrian detector initialized");
    ESP_LOGI(TAG, "Target: pedestrian/person");
    ESP_LOGI(TAG, "PICO input: 224x224, inference runs on CPU1");
}

extern "C" bool yolo_person_submit_frame(const uint8_t *rgb565,
                                          uint16_t width,
                                          uint16_t height)
{
    if (rgb565 == nullptr ||
        width != FRAME_WIDTH ||
        height != FRAME_HEIGHT ||
        s_detector == nullptr ||
        s_frame_queue == nullptr) {
        return false;
    }

    int index = -1;
    if (!find_free_buffer(&index)) {
        return false;
    }

    memcpy(s_frame_buffers[index], rgb565, FRAME_BYTES);

    uint8_t queue_index = static_cast<uint8_t>(index);
    if (xQueueSend(s_frame_queue, &queue_index, 0) != pdTRUE) {
        release_buffer(index);
        return false;
    }

    return true;
}

extern "C" bool yolo_person_get_latest_detection(
    yolo_detection_t *detection)
{
    if (detection == nullptr) {
        return false;
    }

    bool valid;

    portENTER_CRITICAL(&s_result_mux);
    *detection = s_latest_detection;
    valid = s_latest_valid;
    portEXIT_CRITICAL(&s_result_mux);

    return valid;
}

extern "C" bool yolo_person_detect_rgb565(const uint8_t *rgb565,
                                           uint16_t width,
                                           uint16_t height,
                                           yolo_detection_t *detection)
{
    if (rgb565 == nullptr || detection == nullptr || s_detector == nullptr) {
        return false;
    }

    if (width == 0 || height == 0) {
        return false;
    }

    detection->x1 = 0.0f;
    detection->y1 = 0.0f;
    detection->x2 = 0.0f;
    detection->y2 = 0.0f;
    detection->confidence = 0.0f;
    detection->class_id = -1;

    dl::image::img_t image = {
        .data = const_cast<uint8_t *>(rgb565),
        .width = width,
        .height = height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE,
    };

    std::list<dl::detect::result_t> &results = s_detector->run(image);

    bool found_person = false;
    float best_score = 0.0f;

    for (const auto &result : results) {
        if (result.box.size() < 4) {
            continue;
        }

        if (result.score < best_score) {
            continue;
        }

        detection->x1 = static_cast<float>(result.box[0]);
        detection->y1 = static_cast<float>(result.box[1]);
        detection->x2 = static_cast<float>(result.box[2]);
        detection->y2 = static_cast<float>(result.box[3]);
        detection->confidence = result.score;
        detection->class_id = YOLO_PERSON_CLASS;

        best_score = result.score;
        found_person = true;
    }

    return found_person;
}
