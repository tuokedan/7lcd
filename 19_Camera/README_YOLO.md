# ESP32-S3 人体检测

当前 `19_Camera` 使用 Espressif ESP-DL 的轻量级单类人体/行人检测模型 `PICO_S8_V1`。

## 当前链路

OV2640 → 320x240 RGB565 → ESP-DL ImagePreprocessor → PICO_S8_V1 224x224 → 行人检测 → 输出 320x240 原图像素坐标 → LCD 绘制检测框

串口输出示例：

    I (...) yolo: person conf=0.86 bbox=(52,31)-(178,218), infer=xxxx ms

## 为什么不用仓库中的 yolov3tiny.onnx

仓库中的 `yolov3tiny.onnx` 约 34.1 MiB，无法直接部署到当前 8 MB Flash 板卡。

之前测试的 COCO YOLO11n 320x320 INT8 模型虽然可以部署，但 ESP32-S3 上推理时间过长，不适合当前摄像头实时显示链路。

因此当前版本切换为 Espressif 官方的 `pedestrian_detect` / `PICO_S8_V1` 专用人体检测模型。该模型输入为 224x224x3，ESP32-S3 官方参考延迟约为预处理 9.2 ms、模型 118.3 ms、后处理 2.4 ms。

模型文件约 425 KB，适合当前 8 MB Flash + 8 MB PSRAM 的硬件。

## 当前 Flash 分区

`partitions-8MiB.csv` 中：

- `factory`：0x390000（约 3.56 MB）
- `pedestrian_det`：0x100000（1 MB）

人体检测模型独立存放在 `pedestrian_det` 分区，因此修改应用代码时不需要把模型重新编入 app。

## ESP-IDF 版本

当前工程使用 ESP-IDF 5.5.5。

`pedestrian_detect 0.3.2` 依赖 ESP-DL 3.3.x，并支持 ESP32-S3。模型在 Flash partition 模式下要求分区表存在名为 `pedestrian_det` 的分区。

## 编译

进入 `19_Camera`：

    idf.py set-target esp32s3
    idf.py reconfigure
    idf.py build
    idf.py flash monitor

第一次完整 flash 会同时写入 bootloader、partition table、application 和 `pedestrian_det` 人体检测模型。

后续只修改代码时可以优先使用 `idf.py app-flash`，避免反复写入模型分区。

## 当前性能预期

Espressif 官方对 ESP32-S3 的 `pico_s8_v1_s3` 给出的参考值约为：

- 输入：224x224x3
- 预处理：约 9.2 ms
- 模型推理：约 118.3 ms
- 后处理：约 2.4 ms
- 模型文件：约 425 KB

当前固件使用独立 FreeRTOS 任务运行检测，并使用 PSRAM 双缓冲，摄像头和 LCD 不等待 YOLO 推理，从而避免之前 YOLO11n 推理导致主任务看门狗超时。

## 当前功能

1. OV2640 采集 320x240 RGB565
2. LCD 实时显示摄像头画面
3. 后台 CPU1 独立执行人体检测
4. 检测结果输出人体置信度和 320x240 像素坐标
5. LCD 在最近一次有效检测位置绘制红色框
6. 检测忙时自动丢弃待检测帧，不阻塞摄像头/LCD

## 注意

仓库中的 `yolov3tiny.onnx`、原来的 YOLOv3 导出脚本仍然保留，作为 PC 端模型研究资料；它们不参与当前 ESP32-S3 固件编译。
