# Voice Cube — 蓝牙语音输入棒

将 **M5AtomS3R AI Chatbot + Atomic Echo Base** 变成一个蓝牙 Push-to-Talk 语音输入设备，连接 Windows，**流式语音转文字**并自动粘贴到当前输入框。

---

## 硬件

### M5AtomS3R AI Chatbot（主机身）

| 组件 | 规格 |
|------|------|
| **主控** | ESP32-S3-PICO-1-N8R8（双核 240MHz, rev v0.2） |
| **Flash** | 8MB |
| **PSRAM** | 8MB OPI |
| **屏幕** | 0.85" 128×128 彩色 IPS LCD（GC9107，SPI） |
| **背光** | LP5562 驱动（I²C: SDA=GPIO45, SCL=GPIO0） |
| **按键** | GPIO41（中断触发，支持单击/双击/长按） |

### Atomic Echo Base（音频底座）

| 组件 | 规格 |
|------|------|
| **编解码器** | ES8311（低功耗 I2S 音频 codec） |
| **麦克风** | 板载驻极体麦克风（模拟输入） |
| **喇叭** | 板载 0.5W 喇叭（DAC 输出，默认静音） |
| **I/O 扩展** | PI4IOE5V6408（I²C 转 GPIO 0x43，控制 NS4150B 功放） |

### 引脚定义

| 信号 | GPIO | 说明 |
|------|------|------|
| **I2C_SDA** | GPIO38 | ES8311 I²C 数据 |
| **I2C_SCL** | GPIO39 | ES8311 I²C 时钟 |
| **I2S_BCK** | GPIO8 | I²S 位时钟 |
| **I2S_WS** | GPIO6 | I²S 字选择 |
| **I2S_DIN** | GPIO7 | I²S 音频数据输入（麦克风） |
| **I2S_DOUT** | GPIO5 | I²S 音频数据输出（喇叭） |
| **按键** | GPIO41 | 用户按键（按下低电平） |

---

## 项目结构

```
d:\yuyin-fixed/
├── firmware/                     # ESP-IDF 固件
│   ├── main/
│   │   ├── main.c               # 入口 + 状态机（ADVERTISING/IDLE/RECORDING）
│   │   ├── board_config.h       # 引脚定义
│   │   ├── es8311_audio.c/h     # ES8311 编解码器驱动（I²C + 遗留 I2S API）
│   │   ├── es8311_mic.c/h       # 音频采集封装（PCM 累加 + 回调）
│   │   ├── opus_encoder.c/h     # Opus 编码（28kbps, 60ms frames）
│   │   ├── ble_service.c/h      # BLE GATT 服务（NimBLE）
│   │   ├── display.c/h          # GC9107 LCD 驱动 + LP5562 背光
│   │   └── button.c/h           # 按键驱动（GPIO41 中断）
│   ├── sdkconfig.defaults       # 编译配置默认值
│   └── partitions.csv           # 分区表（8MB）
│
├── desktop/python/               # Python 桌面端
│   ├── main.py                  # 入口（PyQt5 + asyncio 双线程）
│   └── voicestick/
│       ├── app.py               # 主应用（托盘图标 + 浮球窗口管理）
│       ├── ble.py               # BLE 客户端（bleak）
│       ├── protocol.py          # BLE 协议解析
│       ├── asr_client.py        # ASR WebSocket 客户端（火山引擎）
│       ├── asr_protocol.py      # 火山引擎二进制协议
│       ├── coordinator.py       # 状态机（BLE + ASR 流式协调）
│       ├── input_injector.py    # 文本注入（SendInput）
│       ├── llm_translation_client.py  # LLM 翻译/润色客户端
│       ├── config.py            # JSON 配置管理
│       └── ui/
│           ├── floatball.py     # 悬浮球交互界面（主球 + 复制 + 功能按钮）
│           ├── pairing.py       # BLE 配对对话框
│           └── settings.py      # 设置对话框
│
├── docs/
│   └── protocol.md              # BLE 协议文档
├── flash_fw.py                  # 编译+烧录脚本
└── monitor.py                   # 串口监视器
```

---

## 快速开始

### 编译固件

```bash
cd D:\yuyin-fixed
python flash_fw.py
```

自动编译并烧录到 COM3。

### 运行桌面端

```bash
cd D:\yuyin-fixed\desktop\python
d:\program\miniconda\python.exe main.py
```

首次需安装依赖：`pip install bleak aiohttp PyQt5`

系统托盘右键 → 设备 → 扫描配对 → 选择 VS-E3F6。

---

## 架构

```
┌─────────────────────────────┐     BLE GATT     ┌────────────────────────────────┐
│  M5AtomS3R + Atomic Echo    │  ◄──────────►   │  Windows 桌面端（Python）      │
│  Base                       │  notify/控制     │                                │
│                             │                  │  BLE 接收 → 流式发送到 ASR     │
│  ES8311 I2S → 16kHz PCM     │                  │  → 火山引擎 ASR → SendInput    │
│  → Opus 编码 → BLE 通知      │                  │  悬浮球 UI + 功能按钮          │
│  按键: 按住录音/松开识别      │                  │  LLM 润色/翻译（可选）          │
│  LCD: 蓝空闲/红录音           │                  │                                │
└─────────────────────────────┘                  └────────────────────────────────┘
```

### 音频流

```
固件                               桌面端
────                                ────
ES8311 → I2S → Opus → BLE notify   BLE → 流式发送到 ASR WebSocket
                                     ├─ 中间结果 → 实时显示
                                     └─ 最终结果 → 自动粘贴
```

### BLE 协议

| Service/Char | UUID | 方向 | 属性 |
|---|---|---|---|
| Voice Stick Service | `8f2f0b84...5100` | — | — |
| audio_tx | `...5101` | 设备 → PC | notify |
| state_tx | `...5102` | 设备 → PC | notify |
| control_rx | `...5103` | PC → 设备 | write/write_no_rsp |

设备名：`VS-XXXX`（基于 MAC 后 2 字节）

---

## 使用指南

### 准备工作

你需要准备两样东西：

| 项目 | 说明 |
|------|------|
| **硬件** | M5AtomS3R + Atomic Echo Base（已烧录本固件） |
| **API Key** | 火山引擎语音识别服务的 APP Key（见下方申请步骤） |

### 申请火山引擎 API Key

Voice Cube 使用**火山引擎语音识别（ASR）**进行转文字，需要免费申请一个 Key：

1. 打开 [火山引擎控制台](https://console.volcengine.com/) → 注册/登录
2. 在搜索栏搜索 **"语音技术"** → 进入产品页 → 点击 **"立即开通"**
3. 在控制台左侧菜单 → **"语音识别"** → **"创建应用"**
4. 创建后进入应用详情，复制 **"APP Key"**（首次注册送数百万帧免费额度，个人使用绰绰有余）
5. ASR 服务器地址默认即可：`wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async`

> **注意：** 如不使用翻译/润色功能，不需要 OpenAI API Key，仅 ASR Key 就可运行。

### 运行桌面端

```bash
cd desktop\python
pip install bleak aiohttp PyQt5
python main.py
```

首次运行后，系统托盘会出现一个蓝色麦克风图标。

### 配对设备

1. 给 AtomS3R 上电（屏幕亮蓝色表示待机）
2. 右键系统托盘图标 → **设备** → **扫描配对**
3. 列表中应出现名为 `VS-XXXX` 或 `VC-XXXX` 的设备，选中后点击连接
4. 配对成功后设备会自动记忆，下次开机自动连接

### 日常使用

**按住说话：**

| 操作 | 效果 |
|------|------|
| 按住按钮 → 说话 | 设备屏幕变红，开始录音并实时发送到 ASR |
| 松开按钮 | 停止录音，ASR 返回识别结果 |
| 结果自动输出到当前光标位置 | 默认使用**模拟粘贴**（Ctrl+V），可在设置中改为**模拟打字** |

**悬浮球功能：**

识别过程中，屏幕上会有一个悬浮球显示实时识别文字。悬浮球上的按钮：

| 按钮 | 功能 |
|------|------|
| ✕ 清除 | 清除当前识别结果 |
| 📋 复制 | 复制识别结果到剪贴板 |
| ✨ 润色 | 调用 LLM 将口语整理成书面语（需在设置中填写 LLM API Key） |
| 🌐 翻译 | 调用 LLM 翻译成目标语言（需在设置中填写 LLM API Key） |
| 💾 保存 | 将结果追加保存到桌面 `notes.md` |

### 设置说明

右键托盘图标 → **设置**：

| 分组 | 选项 | 说明 |
|------|------|------|
| **语音识别** | ASR 服务器 | 火山引擎 WebSocket 地址（默认即可） |
| | API Key | 你申请的火山引擎 APP Key |
| **输入配置** | 自动输出文字 | 打开：识别后自动输出到当前窗口 / 关闭：只显示在悬浮球上 |
| | 末尾自动换行 | 文字输出后自动按一次 Enter，光标跳到下一行 |
| | 输出方式 | **模拟粘贴**：复制到剪贴板 + Ctrl+V（速度快，可能影响剪贴板）<br>**模拟打字**：逐字用键盘发送（不碰剪贴板，部分输入框更兼容） |
| | 已配对设备 | 已配对的设备 ID，逗号分隔 |
| **LLM 大模型** | API 地址 / Key / 模型 | 用于润色和翻译功能的 LLM（可选，兼容 OpenAI 格式的 API） |
| **LLM 润色** | 启用 + 提示词 | 将识别结果整理为书面表达（如去口语化） |
| **翻译** | 启用 + 目标语言 | 将识别结果翻译为目标语言 |

### 输出方式对比

| 特点 | 模拟粘贴 | 模拟打字 |
|------|---------|---------|
| 原理 | 改剪贴板 → Ctrl+V | SendInput Unicode 逐字发送 |
| 速度 | 快（一次性粘贴） | 稍慢（逐字输入） |
| 影响剪贴板 | 会（会自动恢复） | 不会 |
| 特殊场景兼容 | 密码框、受保护编辑框可能被拦截 | 更广泛兼容 |
| 适用场景 | 日常聊天、写文档 | 密码框、Web 表单、终端等 |

---

## 踩坑记录

### 1. ES8311 I2S 无数据 — 新 I2S API 不兼容

**现象：** `i2s_channel_read()` 100% 超时。

**原因：** ESP32-S3 rev v0.2 的 I2S 外设在仅 RX 模式下无法使用新版 `i2s_chan` API（不产生 BCLK/WS）。遗留 `i2s_driver_install` API正常工作。

**修复：** 使用遗留 I2S API：
```c
i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, NULL);
i2s_set_pin(I2S_NUM_0, &pin_cfg);
i2s_read(I2S_NUM_0, buf, len, &br, timeout);
```

### 2. ES8311 时钟分频

**现象：** 有 PCM 数据但全是静音。

**原因：** BCLK 作为 MCLK 时 `pre_multi` 必须为 8（不是 1）。寄存器 `0x02` 值 `0x20` 错误，应为 `0x18`。

**修复：** `{0x02, 0x18}`（pre_div=1, pre_multi=8）

### 3. ES8311 话筒偏置电压

**现象：** 麦克风无信号。

**原因：** 寄存器 `0x15` 设为 `0x00`，未给麦克风提供偏置电压。

**修复：** `{0x15, 0x40}（官方驱动值）`

### 4. 麦克风增益过大/PDM 模式误开

**现象：** 设置 6dB 增益后反而无声。

**原因：** `0x5A` 的 bit6=1 实际启用了 PDM 数字麦克风模式，不是 6dB 模拟增益。正确值 `0x3A`（bit6=0, bit5=1）。

**修复：** `{0x14, 0x3A}`

### 5. Ogg Opus 缺少 lacing values

**现象：** OGG 文件无法播放，ASR 返回格式错误。

**原因：** `page_segments` 设成了整个 packet 长度，且没有写 `lacing_values` 数组。

**修复：** 正确计算 segment 表：
```python
segs = []
rem = len(packet)
while rem > 255: segs.append(255); rem -= 255
segs.append(rem)
page += struct.pack("<B", len(segs))  # page_segments
page += struct.pack(f"<{len(segs)}B", *segs)  # lacing_values
```

### 6. BLE NimBLE 内存池不足

**现象：** 录音中设备随机重启。

**原因：** `MSYS_1_BLOCK_COUNT=12` 远小于官方推荐的 100。音频流式 BLE 通知快速消耗内存池，耗尽后 NimBLE 崩溃。

**修复：** `MSYS_1_BLOCK_COUNT=100`, `MSYS_2_BLOCK_COUNT=0`

### 7. NimBLE 主机栈溢出

**现象：** 第二次录音时设备重启。

**原因：** `BT_NIMBLE_HOST_TASK_STACK_SIZE=4096` 不足，音频通知批量处理时栈溢出。

**修复：** 改为 `8192`

### 8. 棕色检测过于灵敏

**现象：** 设备在录音中无规律重启，有串口监视器时不重现。

**原因：** `BROWNOUT_DET_LVL=7`（最灵敏级），I2S + Opus + BLE 同时运行时 USB 电压微降触发复位。

**修复：** 改为 Level 5，并加 PM lock 防止录音中 CPU 降频/休眠：
```c
esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "recording", &s_pm_lock);
// recording start: esp_pm_lock_acquire(s_pm_lock)
// recording stop:  esp_pm_lock_release(s_pm_lock)
```

### 9. I2S DMA 缓存积累导致突发数据

**现象：** 第二次录音开始时 BLE 通知爆发，NimBLE 崩溃。

**原因：** 录音暂停期间 I2S DMA 持续积累数据，恢复后一次性读取大量 PCM 帧，同时编码+发送导致 NimBLE 压力过大。

**修复：** 恢复录音时先 drain DMA：
```c
if (was_paused) {
    size_t dummy;
    i2s_read(I2S_NUM_0, buf, len, &dummy, 0);  // 丢弃 stale 数据
}
```

### 10. 读任务缓冲区溢出

**现象：** 多次录音后内存损坏。

**原因：** `pcm[960]` 但 `i2s_read` 可返回 1024 个采样，`pcnt` 不归零时写入越界。

**修复：** 裁剪写入量+录音开始时重置 `pcnt`。

### 11. 按键任务优先级导致松开事件丢失

**现象：** 设备卡在红色录音状态。

**原因：** 读任务优先级（6）高于按键任务（5），按键松开事件被饿死。

**修复：** 按键任务优先级改为 7（高于读任务）。

### 12. FreeRTOS 时钟频率

**现象：** 提高 HZ 到 1000 后 vTaskDelay 时序全变。

**原因：** `FREERTOS_HZ=1000` 使 `vTaskDelay(1)` 从 10ms 变成 1ms，各处 delay 实际等待时间缩短 10 倍，引发竞态。

**修复：** 保持 `FREERTOS_HZ=100`。

### 13. OTA 分区类型

**现象：** 烧录后无法启动。

**原因：** `partitions.csv` 类型写错（`app` vs `data`）。

**修复：** 修正 `factory` 分区类型。

### 14. 火山引擎 ASR 会话超时

**现象：** ASR 返回 `[Timeout waiting next packet]` 死循环。

**原因：** ASR 客户端自动重启机制和 `_process_audio` 竞争创建会话。

**修复：** 去掉自动重启，改为每次录音按需新建会话。

### 15. Qt 跨线程 UI 更新

**现象：** `QObject::startTimer: Timers cannot be started from another thread`

**原因：** 浮球 UI 的 `set_status` 从 asyncio 后台线程直接调用了 Qt 定时器操作。

**修复：** 通过 `pyqtSignal` 信号桥编组到 Qt 主线程：
```python
self._bridge.status_signal.emit(status)  # 线程安全
@pyqtSlot(str)
def _on_status_signal(self, s):  # 主线程执行
```

### 16. 桌面端 _recording 标志未复位

**现象：** 第二次说话时不进入录音状态。

**原因：** END 帧处理后 `_recording` 未清 False，下次 button_down 走了错误分支。

**修复：** END 帧时设置 `self._recording = False`。

### 17. BLE 监督超时

**现象：** 约 4 秒规律断连。

**原因：** BLE 连接监督超时（默认约 2-4 秒），音频处理时主机忙，未及时交换数据包。

**修复：** 连接后请求 20 秒监督超时：
```c
struct ble_gap_upd_params params = {
    .itvl_min = 12, .itvl_max = 24,
    .latency = 0, .supervision_timeout = 2000,  // 20秒
};
ble_gap_update_params(conn_handle, &params);
```

---

## 参考链接

| 项目 | 链接 | 用途 |
|------|------|------|
| VoiceStick | https://github.com/78/voicestick | BLE 协议 + 原始参考实现 |
| XiaoZhi ESP32 | https://github.com/78/xiaozhi-esp32 | GC9107 + ES8311 参考 |
| M5AtomS3R 文档 | https://docs.m5stack.com/en/core/AtomS3R | 硬件规格 |
| 火山引擎 ASR | https://www.volcengine.com/docs/6561/1354869 | 语音识别 API |
| ESP-IDF | https://github.com/espressif/esp-idf | ESP32 开发框架 v5.5 |
