#include "yolo_person.h"
#include "esp_log.h"
static const char *TAG="yolo";
void yolo_person_init(void){
 ESP_LOGI(TAG,"YOLOv3-Tiny person detector interface initialized");
 ESP_LOGI(TAG,"COCO class: person (0), camera: %ux%u RGB565",YOLO_CAMERA_WIDTH,YOLO_CAMERA_HEIGHT);
 ESP_LOGW(TAG,"Inference backend is not enabled yet: the .pt PyTorch model must be exported/optimized before ESP32 inference.");
}
bool yolo_person_detect_rgb565(const uint8_t *rgb565,uint16_t width,uint16_t height,yolo_detection_t *detection){
 (void)rgb565;(void)width;(void)height;(void)detection;
 return false;
}
