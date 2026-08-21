# Cardputer NS HID

该固件让 Cardputer（ESP32-S3）通过 USB 模拟 HORI Pokken Controller，
同时连接已有 Wi-Fi 并提供网页控制面板。PC 只负责发送脚本、开始和停止指令；
宏在 Cardputer 本地运行，因此动作时序不受 Wi-Fi 抖动影响。

## 推荐环境

- ESP-IDF 5.5.5
- 目标：`esp32s3`
- 锁定组件：`espressif/esp_tinyusb 1.7.6~2`

```sh
cd usb_hid
cp main/config_example.h main/config.h
# 编辑 main/config.h，填写本地 Wi-Fi SSID 和密码
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash
```

如更换了 ESP-IDF 或目标芯片，请先清理旧的 `build` 目录再构建。
固件关闭了控制台输出；刷写结束后不要启动 `monitor`，直接把 Cardputer
数据口接到 Switch。

## 使用流程

1. 先将固件刷入 Cardputer。
2. 用 Cardputer 的 USB 数据口连接 Nintendo Switch。
3. Cardputer 根据 `main/config.h` 自动连接 Wi-Fi，并通过 DHCP 获取地址。
4. 在路由器后台查找设备名 `Cardputer NS HID` 对应的 IP，然后从 PC 浏览器打开该地址。
5. 如果 NS 显示手柄确认画面，先在网页点“L + R”，短按结束后点“A 确认”。
6. 在《旷野之息》中站到可起跳的边缘，面向发射方向，并预先选中圆形遥控炸弹。
7. 在“宏脚本”标签选择并执行已经验证的风弹或读档预设。
8. 如动作失败，直接修改脚本步骤中的毫秒数后再次测试，不需要重新刷固件。

## Cardputer 实体按键

不需要网页也可以持续按住或反复测试这些 Switch HID 输入：

- `A`、`B`、`X`、`Y`、`L`、`R`：对应 Switch 同名按键。
- `Fn + ;`：上，`Fn + ,`：左，`Fn + .`：下，`Fn + /`：右。
- `Shift + =`：Plus，`Shift + -`：Minus。单独按 `=` 或 `-` 不会发送手柄按键。

实体键在风弹宏或网页短按执行期间暂时让位给自动输入，自动输入结束后立即恢复。

网页显示 USB 是否已经连接、宏是否运行以及当前步骤。执行过程中可点击
“紧急停止”，Cardputer 会立即释放按钮和摇杆。

## 默认风弹序列

```text
ZL[200ms],
ZL+前[200ms],
ZL+前+X[100ms],
null[50ms],
L[100ms],
null[50ms],
ZR[100ms],
null[50ms],
上[100ms],
上+右摇杆右[100ms],
null[50ms],
L[100ms],
null[50ms],
上[200ms],
上+右摇杆左[200ms],
null[50ms],
L[200ms],
null[2000ms],
X[500ms],
null[50ms],
X[500ms],
null[50ms],
X[500ms],
null[50ms],
X[500ms],
null[100ms]
```

每一项代表该时间段内完整的手柄状态，相邻项之间不会自动插入松键。
因此连续几项都含 `ZL` 时，ZL 会一直保持按下。没有写 `[时间]` 的动作默认持续
60ms。网页支持 `A/B/X/Y/L/R/ZL/ZR`、十字键 `上/下/左/右`、`前/后`、
`左摇杆左/右`、`右摇杆左/右` 和 `null`，最多 40 步。

“宏脚本”标签中的读档预设为：`+, R×3, 上×5, 下, A, A, 上, 上, 上, A`。
`×次数` 会展开成多次短按，并自动在相邻按键之间插入松键；次数可直接在网页修改。

“手动脚本”标签用于临时输入并执行自定义状态脚本。不同地形、朝向和游戏运行
状态会影响物理位置，默认脚本只是测试起点。

## HTTP API

- `GET /api/status`：USB 与宏状态。
- `POST /api/script`：执行网页编译后的逐步 HID 脚本。
- `POST /api/stop`：停止并释放所有输入。
- `POST /api/tap?...`：独立短按按钮或方向键，并自动释放。

`/api/script` 接收每行一个步骤的内部数字格式：

```text
duration,buttons,dpad,left_x,left_y,right_x,right_y
```

通常直接使用网页中的中文脚本编辑器，不需要手工生成该数字格式。

## 隐私与安全

- `main/config.h`、`build/` 和 `managed_components/` 已被 Git 忽略；提交前仍建议用
  `git status --short --ignored` 再确认一次。
- Wi-Fi SSID 和密码会以明文形式存在于刷入设备的固件中。不要把使用个人
  `config.h` 构建出的 `.bin` 上传到 GitHub Release；公开二进制应使用专用测试网络配置。
- Web 控制接口只提供 HTTP，当前没有身份验证。请仅把设备连接到可信或隔离的局域网，
  不要通过路由器端口转发、反向代理或公网隧道暴露它。
- `config_example.h` 只能保留占位值，不应写入真实凭据。

## 组件依赖

`main/idf_component.yml` 声明了 `espressif/esp_tinyusb 1.7.6~2`。首次配置或构建时，
ESP-IDF Component Manager 会自动下载到 `managed_components/`；该目录是可再生成的，
不需要上传 GitHub。`dependencies.lock` 应保留在仓库中，以便其他环境解析到
相同的已验证版本。

## 当前协议边界

这是 8 字节 HORI Pokken USB HID 报告，不是完整的 Nintendo Pro Controller
`0x30` 报告协议。它适合按键、方向键和摇杆宏验证，不包含陀螺仪、NFC、震动、
SPI 校准数据或完整的 Nintendo 子命令处理。
