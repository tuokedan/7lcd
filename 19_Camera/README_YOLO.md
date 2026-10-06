# ESP32-S3 人体检测

当前 `19_Camera` 已从原来的 YOLOv3-Tiny 占位接口切换为 Espressif ESP-DL 的 COCO YOLO11n 320x320 INT8 模型。

## 当前链路

OV2640 → 320x240 RGB565 → ESP-DL ImagePreprocessor → YOLO11n 320x320 INT8 → COCO 80 类检测 → 筛选 `person`（class 0）→ 输出 320x240 原图像素坐标

串口输出示例：

    I (...) camera: person conf=0.86 bbox=(52,31)-(178,218), infer=xxxx ms

## 为什么不用仓库中的 yolov3tiny.onnx

仓库中的 `yolov3tiny.onnx` 约 34.1 MiB，无法直接部署到当前 8 MB Flash 板卡。

ESP-DL 官方提供的 `YOLO11N_320_S8_V1` ESP32-S3 INT8 模型约 2.86 MB，并且官方给出了 8 MB Flash + 8 MB PSRAM 的 ESP32-S3 配置。

## 当前 Flash 分区

`partitions-8MiB.csv` 中：
- `factory`：0x390000（约 3.56 MB）
- `coco_det`：0x300000（3 MB）

模型独立存放在 `coco_det` 分区，因此修改应用代码时不需要把模型重新编入 app。

## 重要：ESP-IDF 版本

当前使用的 ESP-DL 3.x / `coco_detect` 组件要求 ESP-IDF 5.3 或更高版本。

本工程之前使用 ESP-IDF 5.1.2；切换本方案后，请将本地 ESP-IDF 切换到 5.3.x。

建议使用 ESP-IDF 5.3 的最新 bugfix 版本。

## 编译

进入 `19_Camera`：

    idf.py set-target esp32s3
    idf.py reconfigure
    idf.py build
    idf.py flash monitor

第一次完整 flash 会同时写入 bootloader、partition table、application 和 `coco_det` YOLO11n INT8 模型。

后续只修改代码时可以优先使用 `idf.py app-flash`，避免反复写入模型分区。

## 当前性能预期

ESP-DL 官方对 ESP32-S3 的 `yolo11n_320_s8_v1_s3` 给出的参考值约为：
- 输入：320x320x3
- Flash：8 MB
- PSRAM：8 MB
- 预处理：约 15.2 ms
- 模型推理：约 6161.8 ms
- 后处理：约 19.3 ms

因此当前版本的第一目标是验证真实人体检测和像素坐标输出，不是实时视频帧率。

后续可以继续优化为：
1. 独立 FreeRTOS 推理任务
2. 摄像头/LCD 与推理解耦
3. 只在固定周期取帧检测
4. LCD 直接绘制 person 框
5. 根据实际速度考虑更小的专用人体检测模型

## 注意

仓库中的 `yolov3tiny.onnx`、原来的 YOLOv3 导出脚本仍然保留，作为 PC 端模型研究资料；它们不参与当前 ESP32-S3 固件编译。
