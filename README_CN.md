# DS5Dongle BL618

基于 BL616/BL618 的 DualSense 无线手柄适配器。将 DualSense / DualSense Edge 手柄通过蓝牙经典（BR/EDR）HID 连接到 BL616/BL618，再通过 USB HID 透传给主机，主机端呈现为标准有线 DualSense（VID 054C / PID 0CE6）或 DualSense Edge（PID 0DF2）。Steam、SDL、PS Remote Play 等均可直接识别。

移植自 [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle)（Raspberry Pi Pico 2W 版本）：BT 栈从 BTstack 迁移至 Bouffalo SDK（Zephyr 内核），USB 栈从 TinyUSB 迁移至 CherryUSB。

> **非官方项目** —— 与索尼互动娱乐（Sony Interactive Entertainment）无关，亦未获其认可。"DualSense"、"DualSense Edge" 和 "PlayStation" 是索尼互动娱乐的商标。USB VID/PID（054C:0CE6 / 0DF2）仅用于模拟，使主机将其识别为有线手柄；使用风险自负。

## 支持的开发板

| 开发板 | 芯片 | USB | LED | 调试 |
|--------|------|-----|-----|------|
| **LCTech BL616**（默认） | BL616 QFN32 | Type-C 原生 | 1x 蓝色（GPIO27） | 外接 USB-TTL |
| **Ai-M61-32S-Kit** | BL618 QFN56 | GPIO37/38 飞线 | RGB + 白色（4 LED） | Type-C CH340 |
| **Sipeed M0S Dock** | BL616 QFN32 | Type-C 原生 | 2x 红色（GPIO27/28） | 外接 USB-TTL |

> **默认构建目标为 LCTech BL616。** `build_macos.sh`（以及 `build_lctech616.sh` 包装脚本）默认即为它构建；其他开发板请使用 `BOARD_TYPE=aim61` 或 `BOARD_TYPE=m0sdock`。

各开发板的 LED 引脚与 USB 拓扑不同，均在编译期通过 `board_config.h` 处理。

## 硬件要求

- **LCTech BL616**、**Ai-M61-32S-Kit** 或 **Sipeed M0S Dock** 开发板
- **DualSense 手柄**（标准版 0CE6）或 **DualSense Edge**（0DF2，自动识别）
- LCTech BL616 / M0S Dock：仅需 USB-C 线（原生 USB）
- Ai-M61-32S-Kit：需 **USB 数据线**（剪线后接 USB_DM/USB_DP 排针）
- LCTech / M0S Dock 串口调试：**USB-TTL 适配器**（CH340/CH341，3.3V）

### USB 接线（仅 Ai-M61-32S-Kit）

板载 Type-C 口是 UART 桥接（烧录/调试用），原生 USB 需要从排针引出：

```
Pin 37 (USB_DM) → USB D-（白色）
Pin 38 (USB_DP) → USB D+（绿色）
GND              → USB GND（黑色）
```

如果主机不通过 USB 供电，开发板需经 Type-C 口单独供电。

> **LCTech BL616 / M0S Dock**：USB 经 Type-C 直连，无需飞线。

## 快速开始

### 1. 安装 Bouffalo SDK（必须使用本项目分支）

> 官方 Bouffalo SDK **无法**用于本项目——缺少固件所需的 BT HID net_buf 池和 USB Audio 请求处理。必须使用本项目维护的 SDK 分支：

```bash
git clone https://github.com/sqlCRT/bouffalo_sdk.git bouffalo_sdk
cd bouffalo_sdk && git checkout 04428daf && cd ..
```

构建脚本要求 SDK 位于 `../bouffalo_sdk`（与本仓库同级目录）。该分支基于上游 v2.3.28，并针对 DS5Dongle BL618 做了针对性修改，详见其 README。

### 2. 安装依赖

```bash
# macOS
brew install cmake
brew install make          # SDK 需要 GNU Make 4.0+

# Linux (Ubuntu)
sudo apt install cmake make
```

### 3. 安装 RISC-V 工具链

BL616/BL618 使用 T-Head 扩展指令集，标准 `riscv64-elf-gcc` 无法使用。

```bash
# macOS ARM64 — 社区预编译 T-Head 工具链
curl -L https://github.com/beckmx/bouffalo_labs_mac_toolchain/releases/download/v1.0/riscv64-unknown-elf-toolchain.tar.gz \
  | tar xz -C ~/riscv-toolchain

# Linux — SDK 自带工具链，或从 T-Head 官方下载
```

`build_macos.sh` 会自动在 `~/riscv-toolchain/toolchain/bin` 下查找工具链。

### 4. 编译

```bash
cd ds5dongle-bl618

# LCTech BL616 — 默认
bash build_macos.sh build      # 增量编译（-j 自动并行）
bash build_macos.sh rebuild    # 清理后重新编译
# 或：bash build_lctech616.sh rebuild

# Ai-M61-32S-Kit
BOARD_TYPE=aim61 bash build_macos.sh rebuild

# Sipeed M0S Dock
BOARD_TYPE=m0sdock bash build_macos.sh rebuild
# 或：bash build_m0sdock.sh rebuild

# USB 速度 — 默认 Full-Speed（线材兼容性最好）；
# 需要 High-Speed 480Mbps 变体时加 USB_SPEED=hs
USB_SPEED=hs bash build_macos.sh rebuild

# 其他命令
bash build_macos.sh clean      # 仅清理
bash build_macos.sh strip      # 删除 .o/.a 中间文件，缩小 build 目录
```

- **默认开发板：LCTech BL616**（`BOARD_TYPE=lctech616`）。
- **默认 USB 速度：Full-Speed（12Mbps）**；传 `USB_SPEED=hs` 可构建 High-Speed 变体（轮询上限更高，但对线材更敏感）。
- 切换 `BOARD_TYPE` 或 `USB_SPEED` 会自动触发一次干净重编（CMake 缓存不兼容）。

产物位于 `firmware/{board}/ds5dongle-{board}.bin`（约 800KB），以及 boot2/partition 文件和 `flash_prog_cfg.ini`。编译产物已被 git 忽略 —— 如需分发预编译固件，建议附加到 GitHub Release。

#### Windows 编译

Windows 下先克隆 T-Head 工具链，然后使用仓库自带的脚本：

```bat
git clone https://gitee.com/bouffalolab/toolchain_gcc_t-head_windows.git

build_windows.bat rebuild     rem LCTech BL616，Full-Speed
build_windows.bat             rem 增量编译
build_windows.bat flash COM5  rem 串口烧录
```

脚本默认要求 DS5Dongle BL618 SDK 分支位于 `..\bouffalo_sdk`（见步骤 1），工具链位于 `%USERPROFILE%\Desktop\toolchain_gcc_t-head_windows`。可通过环境变量 `BL_SDK_BASE` / `TOOLCHAIN_PATH` 覆盖；其他开发板用 `BOARD_TYPE=aim61` / `BOARD_TYPE=m0sdock`，High-Speed 变体用 `USB_SPEED=hs`。

### 5. 烧录

#### LCTech BL616 — Dev Cube（UART/ISP 模式）

1. 先完成编译——构建脚本会把 `ds5dongle-lctech616.bin`、`boot2_bl616_isp_release_v8.1.8.bin` 和 `partition_cfg_4M_nosec.toml` 输出到 `firmware/lctech616/`。
2. 按住 LCTech BL616 的 **BOOT** 键，然后通过 USB-C 插入电脑，保持按住直到开发板进入 UART（ISP）下载模式。
3. 打开 [Bouffalo Lab Dev Cube](https://dev.bouffalolab.com/download)，选择芯片 **BL616**，使用 **ISP（UART）烧录模式**。
4. 选择开发板对应的串口，然后加载以下文件：
   - 分区表：`firmware/lctech616/partition_cfg_4M_nosec.toml`
   - Boot2：`firmware/lctech616/boot2_bl616_isp_release_v8.1.8.bin`
   - 固件：`firmware/lctech616/ds5dongle-lctech616.bin`
5. 开始烧录。

> 请使用 `_nosec` 分区表（`partition_cfg_4M_nosec.toml`），不要使用 `partition_cfg_4M.toml`。

也可以直接用构建脚本烧录：

```bash
bash build_macos.sh flash /dev/tty.usbserial-xxx
```

其他开发板流程相同，使用 `firmware/{board}/` 下对应的文件。

## 使用方法

1. 将 LCTech BL616 开发板的 Type-C 口连接到目标主机（供电 + USB 数据）；如需串口调试，另接 USB-TTL 适配器（CH340/CH341，3.3V）
2. 手柄进入配对模式（同时长按 **PS + Create** 3 秒，灯条闪烁）
3. 观察板载蓝色 LED 状态（见下方 LED 状态表）
4. 主机应识别出 "DualSense Wireless Controller"

> **Ai-M61-32S-Kit 备注：** 其 Type-C 口仅为 UART 桥接（烧录/调试用），原生 USB 需从排针引出（USB_DM/USB_DP/GND）接到目标主机，见上方"USB 接线"。

### LED 状态指示

LCTech BL616 为单颗蓝色 LED（GPIO27），不同状态以闪烁节奏区分：

| 状态 | 模式（单颗蓝灯） |
|------|------------------|
| 空闲 / 等待配对 | 慢闪（~1Hz） |
| 扫描中 | 快闪（~3Hz） |
| 已连接 | 常亮 |
| 刚断开 | 闪烁（~1Hz）约 3 秒，随后回到空闲慢闪 |
| 电量 ≤20%（放电中） | 中速闪（~1.7Hz） |
| 电量 ≤10%（放电中） | 快闪（~3Hz） |
| 自动熄灭 | 1 分钟后熄灭（默认开启；电量告警不受影响） |
| 事件确认 | 闪一次 |
| 清除配对 | 连闪三次 |

> **Ai-M61-32S-Kit 备注（RGB + 白色 LED）：** 空闲 = 紫色慢闪（1Hz）；扫描 = 紫色快闪（~3Hz）；已连接 = 绿色常亮；刚断开 = 红色慢闪，约 3 秒后自动转为紫色；电量 ≤20% = 绿黄色中速闪；电量 ≤10% = 红色中速闪；事件确认 = 蓝色闪一次；清除配对 = 蓝色连闪三次。
>
> **Sipeed M0S Dock 备注（两颗红色 LED，GPIO27/28）：** LED0（靠近 Type-C）用于空闲/扫描闪烁，LED1 在连接后常亮；断开 = 两颗同步闪烁；电量预警 = LED0 快闪；电量严重不足 = 两颗闪烁。

### BOOT 按键手势

| 手势 | 功能 |
|------|------|
| **单击** | 切换到下一个已配对的手柄（最多记忆 8 个） |
| **双击** | 断开当前手柄 + 重新扫描配对新手柄（保留 link key） |
| **长按 3 秒** | 清除所有配对 + 重新扫描（三次蓝闪确认） |

## 功能特性

### 已实现

- BT Classic HID Host：Inquiry、SDP、L2CAP、SSP 自动配对
- 完整输入透传：摇杆、按键、扳机、陀螺仪、加速度计、触摸板、电量
- 完整输出透传：震动、RGB 灯条、玩家指示灯、自适应扳机
- 双向音频透传：UAC1 4ch 48kHz OUT → Opus 编码 → BT 0x39 双帧报告（547B，扬声器/耳机）；BT 麦克风 Opus → 解码 → UAC1 2ch 48kHz IN
- HD 触觉反馈：USB Audio Ch2/Ch3 → 16:1 降采样 → BT 0x92 触觉 tag
- DualSense Edge 完整支持：自动识别 → unlock 握手 → profile 预取 → 437B 描述符，PID 自动切换（0DF2）
- 多手柄记忆：最多记忆 8 个已配对手柄，单击按键快速切换
- 手柄主动回连：L2CAP 服务端注册（被动模式）
- 稳健重连：周期性扫描重试（~30s）、连接看门狗（3s 无输入检测）、链路监督超时（5s）、陈旧 ACL 清理的系统复位兜底
- 空闲超时：可配置 0–60 分钟自动断开（默认 30 分钟）
- PS 快捷键：短按 → Win+G，长按 → Win+Tab
- 轮询率可配置：250Hz / 500Hz / 实时档（~750Hz，跟随 BT 报告率）
- 按键映射：可将手柄任意按键重映射为其他手柄按键（Feature Report 0xFB）
- MuteLight：通过手柄静音键开关麦克风指示灯
- USB 远程唤醒：6 态 FSM + Boot Keyboard + 挂起 5 秒后自动关机
- USB 隐身模式：可配置为手柄连接前不枚举 USB 设备
- 手柄灯条自定义颜色（默认白色）+ 板载 RGB 状态灯
- USB 序列号：eFuse 芯片唯一 ID（可配置，默认开启）
- 扳机电机功率限制：可配置 0–10 档
- 音量锁定：阻止主机修改手柄扬声器/耳机音量
- LED 自动熄灭：常亮 LED 1 分钟后关闭（默认开启；电量告警不受影响）
- 电量告警：≤20% 绿黄闪预警，≤10% 红色闪严重告警
- 多开发板支持：LCTech BL616（默认）、Ai-M61-32S-Kit、Sipeed M0S Dock —— 编译期板型选择
- 构建期日志级别控制（`LOG_LEVEL` 0–3）
- FreeRTOS 多任务架构（BT / USB / Audio / Mic）

### 已知限制

| 项目 | 说明 |
|------|------|
| 单手柄在线 | 同一时刻只能连接一个手柄；最多记忆 8 个配对（单击切换） |

### 手柄功能兼容性

USB 端提供 DualSense 兼容的 HID 描述符（DS 与 Edge 自动切换；标准有线布局 + dongle 配置 Feature Report 0xF6–0xF9 / 0xFB），主机将其视为有线手柄。

| 功能 | 数据路径 | 支持 |
|------|----------|------|
| 摇杆 / 按键 / 扳机 | HID Input 透传 | 是 |
| 陀螺仪 / 加速度计 | HID Input 透传 | 是 |
| 触摸板 | HID Input 透传 | 是 |
| 电池电量 | HID Input 透传 | 是（另有板载 LED 低电量告警） |
| **自适应扳机** | HID Output SetStateData | 是 |
| **震动** | HID Output SetStateData | 是 |
| RGB 灯条 / 玩家指示灯 | HID Output SetStateData | 是 |
| **HD 触觉反馈** | USB Audio Ch2/Ch3 → BT 0x92 | 是 |
| 手柄扬声器 | USB Audio Ch0/Ch1 → Opus → BT 0x39 tag 0x93 | 是 |
| 手柄麦克风 | BT Input → Opus 解码 → USB Audio IN | 是 |
| 3.5mm 耳机（输出） | USB Audio → Opus → BT 0x39 tag 0x96 | 是 |
| 麦克风静音灯 | BT Input 静音键 → MuteLight 控制 | 是 |

## 配置项

配置通过 `bt_settings` 持久化，经 USB Feature Report 0xF6–0xF9 读写，可通过网页免重编译修改（见"网页配置"）。主要配置项（默认值）：

| 配置项 | 默认值 |
|--------|--------|
| 手柄模式 | Auto（DS5 / Edge / Auto） |
| 轮询率 | 250Hz（250 / 500 / 实时 ~750Hz） |
| 空闲自动断开 | 30 分钟（0–60，0 = 关闭） |
| LED 自动熄灭 | 开启（1 分钟后） |
| 灯条自定义颜色 | 白色 |
| USB 序列号 | 开启 |
| USB 隐身模式 | 关闭 |
| PS 快捷键 | 关闭 |
| USB 远程唤醒 | 关闭 |
| 触觉增益 | 1.0（1.0–2.0） |
| 扳机电机限制 | 0（0–10） |
| 音量锁定 | 关闭 |
| 麦克风 / 扬声器透传 | 开启 |

## 项目结构

```
src/
├── main.c              入口 + FreeRTOS 任务编排 + 数据桥接
├── bt_hid_host.c/h     BT Classic HID Host（Inquiry + SDP + L2CAP + SSP）
├── ds5_protocol.c/h    DualSense 协议定义 + CRC32
├── usb_gamepad.c/h     USB 复合设备（Gamepad + Boot Keyboard）
├── ds5_usb_audio.c/h   USB Audio Class 1（4ch 48kHz ISO OUT + 2ch 48kHz ISO IN）
├── audio.c/h           音频处理管线（sinc 重采样 + Opus 编解码 + 触觉 + 麦克风）
├── usb_wake.c/h        USB 远程唤醒 FSM
├── state_mgr.c/h       SetStateData 条件合并管理器
├── config.c/h          配置系统（bt_settings + 0xF6-0xF9）
├── dse.c/h             DualSense Edge Profile 管理
├── remap.c/h           按键映射
├── led_status.c/h      LED 状态指示（Ai-M61 RGB / M0S Dock 双红 / LCTech 单蓝）
├── board_config.h      板级抽象（LED 引脚/极性、USB 类型、板名）
├── debug_log.h         构建期日志级别宏（LOG_ERR/WRN/INF/DBG/ISR）
└── FreeRTOSConfig.h    FreeRTOS 配置
lib/
├── opus/               Opus 编解码库（定点模式，xiph/opus）
├── opus.cmake          Opus 源文件列表
└── opus_config.h       Opus 构建配置
firmware/               板级烧录配置 + 本地编译产物（二进制已 git 忽略）
```

## 架构

```
┌──────────────┐          ┌──────────────┐          ┌──────────────┐
│  DualSense   │◄─ BT ──►│ BL616/BL618  │◄─ USB ──►│   Host PC    │
│  Controller  │  BR/EDR  │ LCTech/M0S/ │  HID     │  Steam/SDL   │
└──────────────┘  HID     │   Ai-M61    │  Device   └──────────────┘
                          └──────────────┘
```

**数据流：**

- **输入（手柄 → 主机）**：BT L2CAP 接收 Report 0x31 → 剥离 HID header/seq/CRC → 63 字节 payload 作为 USB Report 0x01 发送
- **输出（主机 → 手柄）**：USB EP OUT 接收 Report 0x02 → State Manager 条件合并 → BT Report 0x31（78B 含 CRC32）→ L2CAP 发送
- **音频输出（主机 → 手柄）**：USB Audio ISO OUT（4ch 48kHz）→ 双缓冲 PCM 累积 → polyphase sinc 重采样 512→480 → Opus CBR 编码（160kbps）→ 触觉降采样 → 0x39 双帧报告（547B）→ L2CAP 发送
- **音频输入（手柄 → 主机）**：BT 0x31 麦克风 Opus 帧 → 队列 → Opus 解码（48kHz 单声道）→ 单声道转立体声 → 环形缓冲 → USB Audio ISO IN（2ch 48kHz）
- **Feature（双向）**：GET_REPORT 从 BT 侧缓存返回（DSE profile 支持 NAK gating）| SET_REPORT 附加 CRC32 后经 L2CAP 控制通道转发

## 网页配置

配套的网页配置工具出于维护早期用户利益考虑，**不开源**。当前网页地址：[ps5.ds5678.top](https://ps5.ds5678.top)。开源固件仍可通过网页调整各项配置（USB Feature Report 0xF6–0xF9 / 0xFB），但**本开源版本不支持在线 OTA**。有一定动手能力的开发者可以自行实现自己的网页配置页——结合 AI 辅助，实现起来非常简单。

## 致谢

- [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) —— 原始 Pico 2W 实现，核心协议参考
- [bouffalolab/bouffalo_sdk](https://github.com/bouffalolab/bouffalo_sdk) —— BL618 SDK + Zephyr BT 栈
- [CherryUSB](https://github.com/cherry-embedded/CherryUSB) —— USB 协议栈
- [xiph/opus](https://github.com/xiph/opus) —— Opus 音频编解码库（定点模式）
- Linux 内核 `hid-playstation.c` —— DualSense 协议偏移参考
- BL618 移植由 [Cursor](https://www.cursor.com/) + Claude Opus 4.6 协助开发

## 许可证

本项目采用 [GNU General Public License v3.0](LICENSE)（GPL-3.0）许可。任何在分发产品中使用或修改本代码的人，必须以相同许可证开放其源代码。

### 第三方声明

- 代码移植/改编自 [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle)，其采用 MIT 许可证（Copyright (c) 2026 awalol）—— 完整文本见 [NOTICE](NOTICE)。
- [lib/opus](lib/opus) 为 [xiph/opus](https://github.com/xiph/opus) 编解码库，BSD-3-Clause 许可（见 `lib/opus/LICENSE_PLEASE_READ.txt`）。
- [CherryUSB](https://github.com/cherry-embedded/CherryUSB) 与 [bouffalolab/bouffalo_sdk](https://github.com/bouffalolab/bouffalo_sdk) 为外部构建依赖，Apache-2.0 许可。
- Linux 内核 `hid-playstation.c`（GPL-2.0）仅作为协议/偏移参考，未包含内核代码。
