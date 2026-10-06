import torch

# 加载 YOLOv3 模型 (可选: 'yolov3'、'yolov3_spp'、'yolov3_tiny')
model = torch.hub.load("ultralytics/yolov3", "yolov3_tiny", pretrained=True)

# 对图像进行推理 (本地文件、URL、PIL 图像、OpenCV 帧或 numpy 数组)
results = model("https://ultralytics.com/images/zidane.jpg")

# 查看结果
results.print()  # 在控制台打印检测结果
results.show()  # 显示标注后的图像
results.save()  # 将标注图像保存到 runs/detect/exp