# ESP32-S3 → PC 第一版视频传输

这一版先实现单向视频：

OV2640 → ESP32-S3 → Wi-Fi热点 → Windows PC

当前方案不需要第二块 ESP32，也不需要路由器或服务器。

## 1. ESP32端

固件启动后会自动创建 Wi-Fi 热点：

- SSID：ESP32-CAM-Video
- 密码：12345678
- 视频地址：http://192.168.4.1/video
- 浏览器测试地址：http://192.168.4.1/

视频采用 HTTP MJPEG，分辨率为 320×240，当前发送频率限制在约 5 FPS，JPEG 质量为 55。

现有的 RGB565 摄像头链路、LCD 显示和 PICO_S8_V1 人体检测均保留。发送给 PC 的 JPEG 来自当前摄像头帧，因此当前 LCD 上的检测框也会一起出现在 PC 视频中。

## 2. Windows PC端

建议使用 Python 3.10/3.11。

先安装依赖：

    cd E:\7_LCDDisplay11\19_Camera\tools
    python -m pip install -r requirements.txt

然后运行：

    python pc_video_receiver.py

## 3. 使用步骤

1. ESP32 烧录新固件并启动。
2. Windows 连接 Wi-Fi：ESP32-CAM-Video。
3. 密码输入：12345678。
4. 运行 pc_video_receiver.py。
5. PC 会弹出 ESP32-S3 Camera 窗口。
6. 按 Q 或 ESC 退出。

也可以先直接用浏览器访问：

    http://192.168.4.1/

如果浏览器能看到实时画面，说明 ESP32 的视频服务器已经正常工作。

## 4. 第一版的定位

这不是最终的视频通话协议，而是先把最关键的：

摄像头采集 → JPEG编码 → Wi-Fi → PC接收 → 实时显示

完整跑通。

后续在这个基础上再增加：

- PC摄像头 → ESP32 LCD
- 双向视频
- 麦克风/扬声器
- 双向音频

即可逐步变成简易视频对话设备。


## 5. 第二阶段：PC摄像头 → ESP32 LCD

现在已经加入反向视频链路：

PC摄像头 → JPEG → Wi-Fi → ESP32 → JPEG解码 → RGB565 → LCD

ESP32新增接口：

    POST http://192.168.4.1/pcvideo

电脑端运行：

    cd E:\7_LCDDisplay11\19_Camera\tools
    python pc_video_sender.py

使用步骤：

1. ESP32 启动并创建 ESP32-CAM-Video 热点。
2. Windows 连接该热点，密码仍为 12345678。
3. 保持 Python 依赖已安装。
4. 运行 pc_video_sender.py。
5. PC 会显示本机摄像头预览。
6. ESP32 的 ST7789 LCD 应显示 PC 摄像头画面。
7. 按 Q 或 ESC 退出。

当前反向视频约 8 FPS，PC端 JPEG 质量 55，固定发送 320×240。

## 6. 双向视频测试

ESP32 已允许最多 2 个 Wi-Fi 客户端连接，因此后续可以同时运行：

- pc_video_receiver.py：查看 ESP32 摄像头
- pc_video_sender.py：把 PC 摄像头送到 ESP32 LCD

这样就形成：

PC摄像头 → ESP32 LCD
ESP32摄像头 → PC窗口

也就是双向视频的第一版。

当前还没有加入音频；下一阶段再增加麦克风/扬声器和双向音频。
