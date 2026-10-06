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

bool yolo_person_detect_rgb565(const uint8_t *rgb565,
                               uint16_t width,
                               uint16_t height,
                               yolo_detection_t *detection);

#ifdef __cplusplus
}
#endif

#endif
