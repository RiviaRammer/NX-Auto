# NX-Auto

面向 Nintendo Switch 与 Switch 2 的自动化实验平台。项目将 MCU 手柄模拟、
PC 控制工具、视频流处理、计算机视觉和本地实时宏组合在一起，用于构建“采集
→ 识别 → 决策 → 手柄输入”的闭环自动化。

## 项目结构

| 目录 | 作用 | 状态 |
| --- | --- | --- |
| [`usb_hid/`](usb_hid/) | Cardputer / ESP32-S3 通过 USB 模拟 Switch 兼容手柄，提供实体键盘、网页控制和本地宏 | 可用 |
| [`bt_classic_hid/`](bt_classic_hid/) | 使用 Bluetooth Classic HID 模拟第一代 Switch 手柄 | 规划中 |
| [`vision/`](vision/) | 在 PC 或 Tab5 上获取 UVC 视频流，并进行解码、检测、跟踪等视觉处理 | 实验中 |

`vision` 负责视频输入和视觉感知，其输出可作为自动化决策的状态来源。  
Switch 2 的私有 BLE 控制器协议与第一代 Switch 的 Bluetooth Classic HID
不兼容，后续会作为独立子项目接入。

## 快速开始

USB HID 固件的构建、烧录和控制说明见 [`usb_hid/README.md`](usb_hid/README.md)。

PC/Tab5 视频流处理与视觉实验说明见 [`vision/README.md`](vision/README.md)。


## 免责声明

NX-Auto 是独立项目，与 Nintendo 无关。请仅在自己拥有或获得授权的设备上用于
学习、测试和互操作研究。
