# S31 非蓝牙优化同步到 FakePod_Custom

## Context

`FakePod_S31` 从本工程（ESP32-S3）移植过去后，除蓝牙栈（NimBLE → Bluedroid）外还做了 5 组非蓝牙优化。现需把其中与硬件/平台无关的部分同步回 `d:\ESP32\FakePod_Custom\FakePod_Custom`，减少两工程行为差异。

- S31 对应提交：`82da734`、`1943678`、`62cca28`、`e0a1163`、`18fff0e`。
- 本工程平台：ESP32-S3 + ESP-IDF 5.5.3（framework-espidf@3.50503.0），16MB Flash / 16MB PSRAM。

## 同步结论

| 项目 | S31 做法 | S3 现状 | 结论 |
| --- | --- | --- | --- |
| FLAC 工作缓冲与 QoS 日志 | 输入 48KB、96K/192K 预取环 512KB；Emergency 仅曲内首次打印；删除恢复 Normal 日志 | 32KB / 256KB，逐次打印 | 同步 |
| SD 卡频率 | `SDMMC_FREQ_HIGHSPEED`(40MHz) ×2 处 | `SDMMC_FREQ_DEFAULT`(20MHz) | 同步（含风险） |
| RAM 监控日志 | 中文「内存监控：…」 | 英文 `RAM_MON: …` | 同步（仅文案） |
| 触摸 | CST820 硬复位 + 按下连续 3 帧确认 | 无硬复位；按下即上报 | 同步 |
| 硬件 JPEG 解码 | `jpeg_hw_decoder.*` | S3 无 SOC_JPEG_SUPPORTED | 不可同步 |
| PPA 压暗 | `ppa_ops.*` + 64B 对齐 | S3 无 SOC_PPA_SUPPORTED | 不可同步 |
| BoundedSPI IDF 6.1 白名单 | 白名单含 6.1 | 白名单已含 5.5.3 | 无需同步 |
| I2S 高采样率切 APLL | `I2S_CLK_SRC_APLL` | IDF 5.5.3 中 S3 的 `SOC_I2S_CLKS` 无 APLL，且默认 PLL_F160M 已满足 ≤192k | 无法同步（不改） |
| 分区表 | fontcache 4M / spiffs 3968K | fontcache 3M / spiffs 4992K | 暂不同步（需重新分区擦除） |

## 实施步骤

### 1. FLAC 缓冲与 QoS 日志 — `src/audio/decoders/flac_decoder.cpp`

- `FLAC_INPUT_BUFFER_BYTES`：`32768` → `48 * 1024`（L25 附近）。
- `FLAC_PREFETCH_RING_96K_BYTES`、`FLAC_PREFETCH_RING_192K_BYTES`：`256 * 1024` → `512 * 1024`（L34-35 附近）。
  - 这些缓冲均经 `flac_alloc_buffer()` 走 `MALLOC_CAP_SPIRAM`，不占内部 RAM。
- QoS 状态机内（约 L567-591）：
  - Emergency 进入日志外面加 `if (context->qos_emergency_entries == 1U)` 守卫，文案改为「FLAC 预取进入 Emergency（曲内首次）：…」，并补注释说明高码率曲目水位抖动会刷屏。
  - 删除恢复 Normal 的 `ESP_LOGI`，只保留计数 `qos_recovered_count`。

### 2. SD 40MHz — `src/drivers/storage/sdcard.cpp`、`src/system/usb_storage_service.cpp`

- 两处 `host.max_freq_khz = SDMMC_FREQ_DEFAULT;` → `SDMMC_FREQ_HIGHSPEED;`。
- 在 `sdcard.cpp` 处保留回退注释：实机出现访问超时/CRC 错误时改回 `SDMMC_FREQ_DEFAULT`。

### 3. RAM 监控日志中文化 — `src/system/system_loop.cpp`

仅改两条日志字符串（L95-100、L106-115），字段与变量完全不变：

- 低水位：`"内存监控：内部RAM低水位：可用=%uB 最大块=%uB DMA可用=%uB 累计次数=%lu"`
- 60 秒汇总：`"内存监控：60秒汇总：当前内部可用=%uB 60秒最低=%uB 低于12K次数=%lu 累计低于12K=%lu 启动最低=%uB 最大块=%uB DMA可用=%uB 主循环栈剩余=%uB"`

### 4. 触摸硬复位 — `src/drivers/touch/cst820.cpp`

- 新增包含：`driver/gpio.h`、`freertos/FreeRTOS.h`、`freertos/task.h`。
- 新增 `static void cst820_hardware_reset()`（照搬 S31 实现）：
  - `#if defined(FAKEPOD_TOUCH_RST) && FAKEPOD_TOUCH_RST >= 0`
  - `gpio_config` 输出、无上下拉、无中断；失败仅告警返回。
  - 拉低 10ms → 释放 → 延时 80ms 等芯片固件启动。
- 在 `cst820_init()` 的 `i2c_bus_add_device()` 调用之前执行该复位。
- 本工程 `src/board/board_pins.h` 已定义 `FAKEPOD_TOUCH_RST 5`，无需新增引脚宏。

### 5. 触摸按下防抖 — `src/ui/input/touch_input.cpp`

- 新增常量 `TOUCH_PRESS_DEBOUNCE_SAMPLES = 3U`（与 `TOUCH_RELEASE_DEBOUNCE_SAMPLES` 并列）。
- 新增计数 `g_press_glitch_suppressed_count`。
- `touch_input_task()` 局部新增 `press_candidate_samples / press_candidate_x / press_candidate_y`。
- 按下分支改为：
  - 已稳定按下 → 原样发布快照（非边沿）。
  - 未稳定 → 累加候选帧，达到 3 帧才 `stable_pressed = true` 并发布 DOWN 边沿；坐标取最新候选值。
- 未按下且存在候选帧的分支：累加伪按下计数，按 1 次 / 每 32 次节流打印「触摸伪按下已抑制：count=%lu samples=%u last=(%u,%u)」，然后清零候选。
- `ui_touch_input_start()` 中把 `g_press_glitch_suppressed_count` 一并清零。

## 不实施项说明

- **I2S APLL**：IDF 5.5.3 的 `soc/esp32s3/include/soc/clk_tree_defs.h` 中 `SOC_I2S_CLKS = {PLL_F240M, PLL_F160M, XTAL, EXTERNAL}`，无 `I2S_CLK_SRC_APLL`，直接引用会编译失败。S3 默认 `I2S_CLK_SRC_DEFAULT = SOC_MOD_CLK_PLL_F160M`（160MHz），`audio_rate_profile.h` 支持的最高 192kHz 对应 mclk 49.152MHz，满足驱动 `sclk > 1.99 * mclk`，因此无需时钟源回退。
- **分区表 / JPEG / PPA / BoundedSPI 白名单**：见上表，分别因数据风险、芯片能力、已满足而跳过。

## 验证

1. 编译：`pio run -e fakepod_nano`（cwd `d:\ESP32\FakePod_Custom\FakePod_Custom`），确认无告警升级为错误、体积变化可接受。
2. 烧录后看启动日志：
   - 触摸初始化仍能读到 `CST820 芯片 ID：0xB7`，无「注册 CST820 失败」。
   - `内存监控：60秒汇总…` 每 60s 一条，字段齐全。
3. SD：连续扫描曲库 / 打开大封面 / 播放本地 FLAC 5 分钟以上，确认无 `sdmmc` 超时或 `ESP_ERR_TIMEOUT`；异常则回退 20MHz。
4. FLAC：播放 96kHz 曲目 10 分钟，日志中 `FLAC 预取进入 Emergency（曲内首次）` 单曲最多一条，且音频不断流。
5. 触摸：连续快速点按 / 长按 / 滑动 20 次，观察是否还有「触摸伪按下已抑制」与误触；若点击响应主观变慢，可将 `TOUCH_PRESS_DEBOUNCE_SAMPLES` 调回 1。

## 风险

- SD 40MHz 对排线长度/卡质量敏感，是本批改动中唯一有实机稳定性风险的项，已保留回退路径。
- 触摸硬复位会额外占用 GPIO5 输出约 90ms 的初始化时间，仅在启动阶段执行一次。
- FLAC 预取环从 256KB 提到 512KB，单解码器 PSRAM 峰值增加约 256KB；本工程 PSRAM 16MB，余量充足。
