# Bluetooth Classic HID

该子项目用于通过 Bluetooth Classic（BR/EDR）HID 模拟第一代 Nintendo Switch
Joy-Con 或 Pro Controller。

预期范围：

- HID SDP 服务和 L2CAP Control/Interrupt 通道。
- 首次配对、配对信息持久化和自动重连。
- Nintendo 子命令、`0x21` 应答与 `0x30` 输入报告。
- 与 NX-Auto 统一手柄状态、本地宏和上层控制接口对接。

Bluetooth Classic 需要具备 BR/EDR 控制器的芯片，传统 ESP32 可用；ESP32-C3/C6/S3
不适用这条技术路线。Switch 2 使用私有 BLE/GATT 协议，也不属于本目录范围。
