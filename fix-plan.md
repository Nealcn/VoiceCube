# yuyin 固件修复计划

**项目位置**: `/opt/data/yuyin-fixed/firmware/`
**设备**: M5AtomS3R + Atomic Echo Base (ES8311 codec)

---

## 修复项（按实施顺序）

### 1. 🔴 I2S/ES8311 时钟对齐 — 去掉 32kHz→16kHz 软件降采样

**问题**: ESP32 I2S 配 32kHz（BCLK=1.024MHz），ES8311 内部以不同速率处理，然后 pdm_mic_read_task 做简单的 2:1 均值抽取。这种无抗混叠滤波器的降采样会导致高频 aliasing。

**修改文件**:
- `es8311_codec.c` — I2S `clk_cfg` 从 32000 → **16000**，ES8311 寄存器 PLL 参数随 BCLK 变化调整
- `pdm_mic.c` — 删掉 `pdm_mic_read_task` 中的 2:1 降采样代码，I2S 数据直接透传

**方案**: I2S 直接跑 16kHz（BCLK=512kHz），ES8311 PLL 从 512kHz BCLK 锁相。ES8311 的 LRCLK(=WS) = 16kHz = 最终输出采样率。去掉全部软件降采样逻辑。

---

### 2. 🟡 重命名 pdm_mic → es8311_mic

**问题**: 项目已从 PDM 切换到 ES8311 codec，但文件名和 API 名都叫 `pdm_mic`，里面实际调用的是 `es8311_codec_*`，严重误导。

**修改**:
- 将 `pdm_mic.c` → `es8311_mic.c`，`pdm_mic.h` → `es8311_mic.h`
- API: `pdm_mic_init/start/stop/deinit` → `es8311_mic_init/start/stop/deinit`
- `pdm_mic_data_cb_t` → `es8311_mic_data_cb_t`
- `pdm_mic_read_task` → `es8311_mic_read_task`
- `main.c` — 更新所有引用
- `CMakeLists.txt` — 更新 SRCS

---

### 3. 🟡 Opus 编码质量提升

**问题**: `OPUS_SET_COMPLEXITY(0)` = 最差质量。28kbps + complexity=0 下语音明显劣化。

**修改**: `opus_encoder.c` 改 complexity 0 → **2**（ESP32-S3 双核 240MHz 绰绰有余）

---

### 4. 🔴 BLE 断开保护

**问题**: `on_pcm_data` 不检查 BLE 连接状态，断开时 Opus 数据照常编但发不出去。

**修改**: `main.c` — `on_pcm_data` 入口加：
```c
if (!ble_service_is_connected()) {
    stop_recording();
    return;
}
```

---

### 5. 🟡 Read Task Watchdog 保护

**问题**: `pdm_mic_read_task` 录音时循环紧读 I2S，无主动让出 CPU 机制，长录音可能触发 task watchdog。

**修改**: `es8311_mic_read_task` 在读 I2S timeout 时 `vTaskDelay(1)`，以及每读够 N 帧主动 `taskYIELD()`。

---

### 6. 🟡 按钮驱动完善

**问题**: 
- `button_get_press_duration_ms()` 返回 0（桩函数）
- `button_is_pressed()` 返回 false（桩函数）
- 去抖仅 50ms 固定延迟，快速连按可能漏事件

**修改**: `button.c`
- 记录 press_down 时间戳，实现 `button_get_press_duration_ms()`
- 用 GPIO 电平实现 `button_is_pressed()`
- 50ms 去抖后增加一个短时窗口检测双击

---

### 7. 🟢 删除死代码 `led_matrix.c/h`

**问题**: AtomS3R 没有 5×5 RGB LED 矩阵，但 `led_matrix.c/h` 仍在仓库里。

**修改**: 删除 `led_matrix.c` 和 `led_matrix.h`，`CMakeLists.txt` 中移除。

---

### 8. 🟢 分区表使用 8MB Flash

**问题**: AtomS3R 有 8MB flash，分区表只用了前 4MB，后 4MB 浪费。

**修改**: `partitions.csv` + `sdkconfig`：
- Flash size: 4MB → **8MB**
- factory: 0x300000 → 0x500000（5MB，给固件更多空间）
- storage: 保持适当大小

---

### 9. 🟢 CMakeLists.txt 清理

**问题**: REQUIRES 包含重复/不存在的条目（`esp_driver_spi`, `esp_driver_i2c` 在 ESP-IDF 5.x 中已合并到 `driver`）

**修改**: `firmware/main/CMakeLists.txt` 精简 REQUIRES 为实际需要的组件。

---

## 执行顺序

```
Step 1: CMakeLists 清理 + 删 led_matrix       (无风险，先清理)
Step 2: 重命名 pdm_mic → es8311_mic            (改名，不影响逻辑)
Step 3: I2S 时钟 16kHz 直配+去掉降采样           (核心修复)
Step 4: Opus complexity 0→2                    (简单改参数)
Step 5: BLE 断开保护                            (加几行检查)
Step 6: Button 完善 + Watchdog 保护             (功能增强)
Step 7: 分区表 8MB                             (最后改，不影响代码)
```

每步完成后确认编译通过再进下一步。
