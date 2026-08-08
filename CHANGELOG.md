# Changelog

All notable changes to DS5Dongle BL618 firmware are documented here.

---

## v3.15 — 2026-08-06

### Added
- 新增自定义手柄连接后灯光颜色，可在配置页面选择

### Fixed
- 修复调整系统音量后手柄灯光熄灭的问题，修复绝区零角色 LED 灯光熄灭问题
- 修复第二次进入游戏后手柄 LED 灯光不亮的问题
- 修复 Steam 输入转换游戏中震动延迟和持续震动的问题
- 可能修复切换手柄时第二个手柄音频断断续续的问题
- 修复断开后重连时看门狗误触发导致立即断联
- 修复音频流关闭后 USB 端点状态残留，提升传输稳定性
- 修复电脑待机恢复后手柄可能无响应的问题

### Changed
- 降低空闲时功耗和发热
- 麦克风缓冲区扩容，减少爆音

---

## v3.14 — 2026-08-02

### Added
- 新增双版本固件：普通版（兼容性优先）和高轮询率版（最高 750Hz）

### Changed
- 默认固件切换为普通版（Full-Speed），USB 线材兼容性更好

### Improved
- 优化音频编码性能，降低 CPU 开销

---

## v3.13 — 2026-08-01

### Improved
- 优化重连成功率

---

## v3.12 — 2026-07-30

### Added
- 支持记忆多个手柄（最多 8 个），单击按钮快速切换
- 游戏中手柄断连重连后，自动恢复自适应扳机状态

### Improved
- 大幅优化蓝牙配对和重连效率，手柄从其他设备切换回来无需手动清除

### Changed
- 双击按钮改为搜索新手柄配对（原：软重启）
- USB 速度上限调整为 1000Hz

### Fixed
- 可能优化了扳机响应速度

---

## v3.11 — 2026-07-29

### Added
- 实时档位轮询率从 ~500Hz 提升至 ~750Hz

### Changed
- 连接成功后手柄灯光默认改为白色
- 默认开启 USB 隐身模式
- 默认开启 1 分钟后自动关闭指示灯

### Fixed
- 修复高负载下可能崩溃的问题
- 优化蓝牙重连响应速度，减少手柄低电量断开后重连失败的概率

---

## v3.10 — 2026-07-25

### Fixed
- 优化陀螺仪数据传输稳定性，减少瞄准时的抖动
- 更新 SDK 蓝牙控制器库至 v2.3.30-RC1

---

## v3.9 — 2026-07-24

### Added
- 按键映射（Button Remap）功能：支持将手柄任意按键重映射到其他手柄按键（暂不支持键盘映射）
- 新增 HID Feature Report 0xFB 用于 Web 配置工具读写映射表
- Web 配置界面新增控制器可视化面板，点击按键可设置映射；支持按实体手柄键直接捕获目标键
- Web 配置页面改为 Tab 布局（配置 / 按键映射 / 操作说明），新增配对操作说明

### Fixed
- 修复首次连接失败率高：缩短各超时（CONNECTING 15s→8s，DISCONNECTING 5s→1.5s，L2CAP CFG 4s→2s）
- OTA 版本字符串修复（原误显示为 event_v1.1.1）
- 音量映射修正：Windows dB 范围正确映射到 DualSense [0,127]，解决音量偏小问题
- 键盘接口仅在 PS 快捷键启用时包含，避免额外 USB 接口干扰游戏自适应板机和音频
- 修复 USB 唤醒：无键盘接口时正确设置 REMOTE-WAKEUP 标志位

---

## v3.8 — 2026-07-21

### Fixed
- 修复 Linux（Bazzite / PipeWire）下扬声器播放卡顿的问题
- 修复播放音乐时不操作手柄会自动断开连接的问题

---

## v3.7 — 2026-07-20

- Earlier LED primer + stealth mode purple on reconnect
- Stealth primer hold-forward logic and primer logs

## v3.6 and earlier

See git log for details.
