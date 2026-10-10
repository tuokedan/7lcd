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
static constexpr uint16_t MODEL_SIZE = 224;
static constexpr size_t MODEL_BYTES =
    static_cast<size_t>(MODEL_SIZE) * MODEL_SIZE * 2;
static constexpr int BUFFER_COUNT = 2;

/* 当前 PICO_S8_V1 单次推理约 300 ms。限制送入频率，避免 CPU1 连续满载。 */
static constexpr int64_t DETECT_INTERVAL_US = 500000; /* 约 2 FPS */
static int64_t s_last_submit_us = 0;

static PedestrianDetect *s_detector = nullptr;
static uint8_t *s_frame_buffers[BUFFER_COUNT] = {nullptr, nullptr};
static uint8_t *s_model_buffer = nullptr;

/* 0=空闲，1=已排队，2=正在推理。 */
static volatile uint8_t s_buffer_state[BUFFER_COUNT] = {0, 0};
static QueueHandle_t s_frame_queue = nullptr;

static portMUX_TYPE s_result_mux = portMUX_INITIALIZER_UNLOCKED;
static yolo_detection_t s_latest_detection = {};
static bool s_latest_valid = false;
static int64_t s_latest_result_us = 0;
static constexpr int64_t DETECTION_MAX_AGE_US = 900000;

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
        s_latest_result_us = esp_timer_get_time();
    } else {
        s_latest_result_us = 0;
    }
    s_latest_valid = valid;
    portEXIT_CRITICAL(&s_result_mux);
}

static inline void rgb565_black(uint8_t *dst)
{
    dst[0] = 0;
    dst[1] = 0;
}

/*
 * OV2640: 320x240 (4:3)
 * PICO_S8_V1: 224x224 (1:1)
 *
 * 采用等比例缩放：
 *   320x240 -> 224x168
 *   上下各留 28 像素黑边
 *
 * 这样模型看到的目标几何比例与原始画面一致，检测框再反算回
 * 320x240 时不会因为 4:3 -> 1:1 拉伸而产生系统性偏移。
 */
static void make_letterbox_rgb565(const uint8_t *src,
                                  uint16_t width,
                                  uint16_t height,
                                  uint8_t *dst)
{
    const float scale = (MODEL_SIZE * 1.0f) /
                        ((width > height) ? width : height);
    const int resized_w = static_cast<int>(width * scale + 0.5f);
    const int resized_h = static_cast<int>(height * scale + 0.5f);
    const int pad_x = (MODEL_SIZE - resized_w) / 2;
    const int pad_y = (MODEL_SIZE - resized_h) / 2;

    for (size_t i = 0; i < MODEL_BYTES; i += 2) {
        rgb565_black(dst + i);
    }

    for (int dy = 0; dy < resized_h; ++dy) {
        int sy = static_cast<int>((dy + 0.5f) / scale);
        if (sy >= height) sy = height - 1;

        for (int dx = 0; dx < resized_w; ++dx) {
            int sx = static_cast<int>((dx + 0.5f) / scale);
            if (sx >= width) sx = width - 1;

            const size_t src_off =
                (static_cast<size_t>(sy) * width + sx) * 2;
            const size_t dst_off =
                (static_cast<size_t>(dy + pad_y) * MODEL_SIZE +
                 (dx + pad_x)) * 2;

            dst[dst_off] = src[src_off];
            dst[dst_off + 1] = src[src_off + 1];
        }
    }
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
            ESP_LOGD(TAG, "no person, infer=%lld ms",
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

    s_model_buffer = static_cast<uint8_t *>(
        heap_caps_malloc(MODEL_BYTES,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (s_model_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate 224x224 model buffer");

        for (int i = 0; i < BUFFER_COUNT; ++i) {
            heap_caps_free(s_frame_buffers[i]);
            s_frame_buffers[i] = nullptr;
        }

        delete s_detector;
        s_detector = nullptr;
        return;
    }

    s_frame_queue = xQueueCreate(BUFFER_COUNT, sizeof(uint8_t));
    if (s_frame_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create frame queue");

        for (int i = 0; i < BUFFER_COUNT; ++i) {
            heap_caps_free(s_frame_buffers[i]);
            s_frame_buffers[i] = nullptr;
        }
        heap_caps_free(s_model_buffer);
        s_model_buffer = nullptr;

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
        heap_caps_free(s_model_buffer);
        s_model_buffer = nullptr;

        delete s_detector;
        s_detector = nullptr;
        return;
    }

    ESP_LOGI(TAG, "Pedestrian detector initialized");
    ESP_LOGI(TAG, "Target: pedestrian/person");
    ESP_LOGI(TAG, "PICO input: 224x224 letterbox, inference runs on CPU1");
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

    const int64_t now_us = esp_timer_get_time();
    if (s_last_submit_us != 0 &&
        now_us - s_last_submit_us < DETECT_INTERVAL_US) {
        return false;
    }
    s_last_submit_us = now_us;

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
    int64_t result_us;

    portENTER_CRITICAL(&s_result_mux);
    *detection = s_latest_detection;
    valid = s_latest_valid;
    result_us = s_latest_result_us;
    portEXIT_CRITICAL(&s_result_mux);

    if (!valid || result_us == 0) {
        return false;
    }

    /*
     * 推理约 300~400 ms。超过这个时间的结果很容易对应到明显更早
     * 的画面，宁可暂时不画框，也不要把旧框叠到新画面上。
     */
    if (esp_timer_get_time() - result_us > DETECTION_MAX_AGE_US) {
        return false;
    }

    return true;
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

    if (s_model_buffer == nullptr) {
        return false;
    }

    make_letterbox_rgb565(rgb565, width, height, s_model_buffer);

    dl::image::img_t image = {
        .data = s_model_buffer,
        .width = MODEL_SIZE,
        .height = MODEL_SIZE,
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

        /*
         * ESP-DL 的 dl::detect::result_t::box 格式为：
         * [left_up_x, left_up_y, right_down_x, right_down_y]，
         * 即 [x1, y1, x2, y2]，不是 [x, y, width, height]。
         * 先按 xyxy 读取，再去掉 224x224 letterbox 上下各 28 px
         * 的 padding，最后映射回摄像头 320x240 原图坐标。
         */
        constexpr float scale = 224.0f / 320.0f;
        constexpr float pad_x = 0.0f;
        constexpr float pad_y = 28.0f;

        const float model_x1 = static_cast<float>(result.box[0]);
        const float model_y1 = static_cast<float>(result.box[1]);
        const float model_x2 = static_cast<float>(result.box[2]);
        const float model_y2 = static_cast<float>(result.box[3]);

        /* 排除模型输出中坐标顺序异常或退化的框。 */
        if (model_x2 <= model_x1 || model_y2 <= model_y1) {
            ESP_LOGW(TAG, "Reject invalid raw box=[%d,%d,%d,%d] score=%.2f",
                     result.box[0], result.box[1],
                     result.box[2], result.box[3], result.score);
            continue;
        }

        float x1 = (model_x1 - pad_x) / scale;
        float y1 = (model_y1 - pad_y) / scale;
        float x2 = (model_x2 - pad_x) / scale;
        float y2 = (model_y2 - pad_y) / scale;

        if (x1 < 0.0f) x1 = 0.0f;
        if (y1 < 0.0f) y1 = 0.0f;
        if (x2 > width - 1) x2 = width - 1;
        if (y2 > height - 1) y2 = height - 1;

        /*
         * 将检测框以中心点为基准缩小到原宽、高的 50%。
         * 例如原框 [10,10,240,230]，中心点为 (125,120)，
         * 缩小后约为 [67.5,65,182.5,175]。
         * 保持中心点不变，避免缩框影响跟随方向判断。
         */
        const float original_x1 = x1;
        const float original_y1 = y1;
        const float original_x2 = x2;
        const float original_y2 = y2;
        const float center_x = (original_x1 + original_x2) * 0.5f;
        const float center_y = (original_y1 + original_y2) * 0.5f;
        const float half_w = (original_x2 - original_x1) * 0.25f;
        const float half_h = (original_y2 - original_y1) * 0.25f;

        x1 = center_x - half_w;
        x2 = center_x + half_w;
        y1 = center_y - half_h;
        y2 = center_y + half_h;

        if (x1 < 0.0f) x1 = 0.0f;
        if (y1 < 0.0f) y1 = 0.0f;
        if (x2 > width - 1) x2 = width - 1;
        if (y2 > height - 1) y2 = height - 1;

        const float box_w = x2 - x1;
        const float box_h = y2 - y1;
        const float box_area = box_w * box_h;
        const float image_area = static_cast<float>(width) * height;

        /*
         * 缩框后仍覆盖几乎全宽、全高或面积超过画面 75% 时，
         * 仍视为异常框并过滤。
         */
        if (box_w <= 1.0f || box_h <= 1.0f ||
            box_w >= width * 0.95f ||
            box_h >= height * 0.95f ||
            box_area >= image_area * 0.75f) {
            ESP_LOGW(TAG,
                     "Reject oversized box raw=[%d,%d,%d,%d] shrunk=(%.0f,%.0f)-(%.0f,%.0f) score=%.2f",
                     result.box[0], result.box[1],
                     result.box[2], result.box[3],
                     x1, y1, x2, y2, result.score);
            continue;
        }

        ESP_LOGI(TAG, "Raw box=[%d,%d,%d,%d] -> mapped=(%.0f,%.0f)-(%.0f,%.0f) shrunk=(%.0f,%.0f)-(%.0f,%.0f) score=%.2f",
                 result.box[0], result.box[1],
                 result.box[2], result.box[3],
                 original_x1, original_y1, original_x2, original_y2,
                 x1, y1, x2, y2, result.score);

        detection->x1 = x1;
        detection->y1 = y1;
        detection->x2 = x2;
        detection->y2 = y2;
        detection->confidence = result.score;
        detection->class_id = YOLO_PERSON_CLASS;

        best_score = result.score;
        found_person = true;
    }

    return found_person;
}
