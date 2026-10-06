#include "yolo_person.h"

#include "coco_detect.hpp"
#include "dl_image_define.hpp"
#include "esp_log.h"

#include <list>

static const char *TAG = "yolo";

static COCODetect *s_detector = nullptr;

extern "C" void yolo_person_init(void)
{
    if (s_detector != nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Initializing ESP-DL COCO YOLO11n 320x320 INT8 detector...");

    s_detector = new COCODetect(COCODetect::YOLO11N_320_S8_V1);

    if (s_detector == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate COCODetect");
        return;
    }

    ESP_LOGI(TAG, "ESP-DL detector initialized");
    ESP_LOGI(TAG, "Target class: person (COCO class 0)");
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
        .width = static_cast<int>(width),
        .height = static_cast<int>(height),
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565,
    };

    std::list<dl::detect::result_t> &results = s_detector->run(image);

    bool found_person = false;
    float best_score = 0.0f;

    for (const auto &result : results) {
        if (result.category != YOLO_PERSON_CLASS || result.box.size() < 4) {
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
        detection->class_id = result.category;

        best_score = result.score;
        found_person = true;
    }

    return found_person;
}
