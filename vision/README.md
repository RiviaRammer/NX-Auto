# Nintendo Switch 图像采集

NS 底座的 HDMI OUT 接入 USB 2.0 采集卡的 HDMI IN。采集卡的 USB 只能接一个
Host：调试 PC 方案时接 PC，调试 Tab5 方案时接 Tab5 USB-A，不能同时连接。

```text
Nintendo Switch（插入底座） -> HDMI -> USB 2.0 UVC 采集卡 -> PC 或 Tab5
```

> Switch Lite 本身不能通过底座输出 HDMI。普通版/OLED 版需要给底座正常供电；
> 系统设置中先固定为 720p，关闭“与电视电源状态同步”，可减少重新握手。

## 1. 先在 PC 验证采集卡

绝大多数免驱采集卡是 UVC 设备。先关闭 OBS、相机等可能独占设备的程序，然后：

```sh
cd capture/pc
python -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
python ns_capture.py --device 0 --width 1280 --height 720 --fps 30
```

窗口中按 `S` 保存原始帧，按 `Q` 退出。程序启动时会打印采集卡最终接受的
分辨率、帧率和 FourCC；这些“实际参数”比商品页标称值更重要。

常用命令：

```sh
# Windows：DirectShow 通常更容易请求到 MJPEG
python ns_capture.py --device 0 --backend dshow

# Linux：设备也可以直接写路径
python ns_capture.py --device /dev/video2 --backend v4l2

# 无窗口，每 2 秒保存一帧，保存 300 帧后退出
python ns_capture.py --headless --save-every 2 --max-frames 300
```

Linux 可先用下面的命令取得板端所需的格式表：

```sh
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video2 --list-formats-ext
```

如果 720p 黑屏，依次测试 `--width 640 --height 480 --fps 15`、更换 USB
端口、重新给 NS 底座上电。若 `MJPG` 不在格式表中，PC 可改用 `--fourcc YUY2`，
但这种采集卡通常不适合直接接微控制器。

### YOLO 人物检测与跟踪

确认 `/dev/video2` 可用后，在安装了 OpenCV、Ultralytics 的同一 Python 环境运行：

```sh
cd capture/pc
source .venv/bin/activate
python ns_person_track.py --source /dev/video2
```

默认使用轻量 `models/yolo11n.pt`，只保留 COCO 的 `person` 类别，并通过 ByteTrack 给连续
画面中的人物分配 ID。首次使用某个官方模型时，Ultralytics 可能需要联网下载权重。
按 `S` 保存带检测框的截图，按 `Q` 退出。

如果自动下载失败，直接从 Ultralytics 官方发布页下载：

- `yolo11n.pt`：<https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n.pt>
- `yolo11s.pt`：<https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11s.pt>

保存到 `capture/pc/models/`，例如完整路径为
`capture/pc/models/yolo11s.pt`。随后显式指定 `--model models/yolo11s.pt`。

```sh
# NVIDIA GPU 0，并保存带框录像
python ns_person_track.py --source /dev/video2 --compute-device 0 \
  --record captures/ns_people.mp4

# CPU 性能不足时降低推理分辨率；采集仍保持 720p
python ns_person_track.py --source /dev/video2 --compute-device cpu --imgsz 416

# 使用新的官方模型或自己的游戏角色模型
python ns_person_track.py --source /dev/video2 --model yolo26n.pt
python ns_person_track.py --source /dev/video2 \
  --model runs/detect/train/weights/best.pt --classes all
```

这里的“人物检测”表示判断画面中哪里像一个人，不等于识别人物身份。通用 COCO
权重主要来自现实照片，对林克、NPC、怪物等卡通游戏角色可能漏检。要可靠地区分
“林克 / NPC / 敌人 / Boss”，应从实际 NS 画面截帧、标注这些自定义类别，再训练
YOLO 检测模型；不建议一开始就做人脸身份识别。

## 2. Tab5 直接采集并显示

`tab5` 是独立 ESP-IDF 工程，不会改变仓库现有 Cardputer HID 固件。推荐
ESP-IDF 5.5.x：

```sh
cd capture/tab5
idf.py set-target esp32p4
idf.py build
idf.py -p PORT flash monitor
```

使用 VS Code ESP-IDF 插件时，建议通过 **File → Open Folder** 单独打开
`capture/tab5`，避免与仓库中目标为 ESP32-S3 的 `usb_hid` 工程混用配置。随后依次执行：

1. `ESP-IDF: Set Espressif Device Target` → `esp32p4`。
2. `ESP-IDF: Select Port to Use` → Tab5 对应串口。
3. 状态栏或命令面板中的 `Build Project`、`Flash Device`、`Monitor Device`。

ESP-IDF 插件打开的终端会自动载入 IDF 环境；普通系统终端没有 `idf.py` 属于正常
现象。

首次构建会由 ESP Component Manager 下载官方 `m5stack_tab5_noglib` BSP、UVC Host
和 JPEG 解码组件，因此构建机需要联网。

刷写后，给 Tab5 正常供电，把采集卡插入 **USB-A Host**。固件会打开 USB-A
VBUS，匹配任意 VID/PID 的第一个 UVC 流，接收 MJPEG 320x240@15fps，硬件解码为
RGB565 并居中显示。串口出现 `NS 图像流已启动` 即表示采集成功。

首版故意使用较低模式来提高兼容性。确认稳定后，可修改
`main/main.c` 顶部的 `CAPTURE_WIDTH`、`CAPTURE_HEIGHT`、`CAPTURE_FPS`；建议下一档
先试 640x480@15，而不是直接上 1280x720。分辨率必须是采集卡 UVC 描述符中真实
存在的 MJPEG 模式，否则 `uvc_host_stream_open()` 会失败。

### Tab5 常见失败

- 一直“打开失败”：采集卡不是 UVC、没有 MJPEG 320x240 模式，或 UVC 视频流
  不是索引 0。先在 PC 导出格式表。
- 能枚举但无画面：短 USB 线重试；采集卡耗电较大时，使用带外部供电的 USB 2.0
  Hub，避免让 Tab5 独自承担其 5V 峰值电流。
- `FRAME_BUFFER_OVERFLOW`：增大 `MJPEG_FRAME_BYTES`，或降低分辨率/帧率。
- 颜色异常：确认请求的是 MJPEG；当前固件不处理 YUY2/UYVY。
- 需要同时保留 HID 控制：Cardputer 继续通过 USB 接 NS；Tab5/PC 只接 HDMI
  采集卡。两条链路物理上独立，最稳妥。

## 验收顺序

1. NS 底座直连电视，确认 HDMI 输出正常。
2. PC 工具显示画面并保存一张截图，记录实际 FourCC/分辨率/FPS。
3. 确认格式表包含 MJPEG 320x240 或 640x480。
4. 再把采集卡 USB 改接 Tab5，观察串口枚举和首帧日志。
5. 低分辨率稳定后再提升画质，并在后续图像识别中消费解码后的 RGB565 帧。
