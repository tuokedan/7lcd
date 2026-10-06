# YOLOv3-Tiny person detection

The working camera/LCD path remains 320x240 RGB565. This component provides a stable interface for the embedded detector.

The repository checkpoint yolov3-tiny.pt is a PyTorch model and cannot be executed directly by ESP32-S3. It must first be exported and optimized for an embedded inference backend.

Target pipeline:
OV2640 320x240 RGB565 -> preprocessing -> YOLOv3-Tiny -> NMS -> COCO class 0 person -> map bbox to 320x240 -> ESP_LOGI.

Example target output:
YOLO: person conf=0.87 bbox=(72,35)-(168,224)

The current component deliberately returns no fake detections. Once the embedded backend is selected, only yolo_person_detect_rgb565() needs to be connected to it.
