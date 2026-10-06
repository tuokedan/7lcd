#ifndef YOLO_PERSON_H
#define YOLO_PERSON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YOLO_CAMERA_WIDTH  320
#define YOLO_CAMERA_HEIGHT 240
#define YOLO_PERSON_CLASS  0

typedef struct {
    float x1;
    float y1;
    float x2;
    float y2;
    float confidence;
    int class_id;
} yolo_detection_t;

void yolo_person_init(void);

/*
 * 将一帧 RGB565 图像复制到 YOLO 后台任务的 PSRAM 缓冲区。
 * 如果后台正在推理且没有空闲缓冲区，则直接返回 false，不阻塞摄像头/LCD。
 */
bool yolo_person_submit_frame(const uint8_t *rgb565,
                              uint16_t width,
                              uint16_t height);

/* 获取最近一次有效的人物检测结果。 */
bool yolo_person_get_latest_detection(yolo_detection_t *detection);

/* 保留同步接口，便于后续单独测试模型。 */
bool yolo_person_detect_rgb565(const uint8_t *rgb565,
                               uint16_t width,
                               uint16_t height,
                               yolo_detection_t *detection);

#ifdef __cplusplus
}
#endif

#endif
