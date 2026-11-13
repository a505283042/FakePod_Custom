# FakePod Custom

基于 ESP32-S3 的便携播放器定制固件。以 460×460 AMOLED 圆角屏幕为核心，提供本地/NAS 音乐播放、视频播放、电子书阅读、NSF 电子音流、图片浏览、BLE 遥控与手柄、USB 读写 TF 卡等能力，全部操作围绕触摸、两个实体键、体感手势完成。

固件版本单一入口见 [src/system/app_build_info.h](src/system/app_build_info.h)，当前为 **R149.3**。

---

## 目录

- [功能特性](#功能特性)
- [硬件平台](#硬件平台)
- [目录结构](#目录结构)
- [快速开始](#快速开始)
- [构建环境](#构建环境)
- [版本基线](#版本基线)
- [完整固件打包与离线烧录](#完整固件打包与离线烧录)
- [TF 卡目录约定](#tf-卡目录约定)
- [视频子系统](#视频子系统)
- [工具](#工具)
- [主机回归测试](#主机回归测试)
- [开发约定](#开发约定)
- [常见问题](#常见问题)
- [相关文档](#相关文档)

---

## 功能特性

| 模块 | 能力 |
|---|---|
| 音乐播放 | MP3 / FLAC（最高验证 96kHz）/ WAV / Opus；音频解码在独立任务，播放时钟与进度条同步 |
| 曲库组织 | 总列表、一级列表、二级列表三种范围；歌曲 / 歌手 / 专辑 / 年代四类分组；拼音首字母快速搜索 |
| 播放控制 | 顺序、列表循环、单曲循环、随机；上一首 / 下一首 / 拖动进度 / 音量 / 静音 |
| 视觉呈现 | 歌词页、三种频谱样式、磁带模式（外壳跟随封面主色自动着色）、封面位置微调 |
| 媒体来源 | TF 卡 `/MUSIC`；NAS（WiFi + HTTP 流 + 目录缓存，可切换服务器） |
| 视频 | AVI（MJPEG + MP3）经 esp_extractor 解封装，BoundedSPI 直出 QSPI，支持手势快进/快退、双击暂停、FVIX 索引精确定位 |
| 电子书 | `/txt` UTF-8 TXT，分页、书签、全屏阅读、阅读位置记忆、页索引缓存 |
| 电子音流 | `/synth` NSF 播放，内置合成器与专用数字补偿 |
| 图片浏览 | 扫描 SD 卡内 JPG / PNG，翻页浏览 |
| 系统 | 锁屏 / AOD 息屏 / 全黑熄屏、亮度、自动熄屏、输出模式（普通耳机 / 高阻耳机 / 线路输出）、辅助键模式 |
| 交互 | 触摸手势、电源键、辅助键 K1、体感翻转与双击、BLE 遥控 / BLE 手柄（十字键与多媒体键） |
| 连接 | USB TinyUSB MSC + CDC：电脑直接管理 TF 卡；BLE 双角色（手机 GATT 外设 / 手柄 HID 主机，运行期互斥） |
| 电源 | 电池电量采集、机身姿态检测、关机前保存现场 |

完整操作说明见 [FakePod 使用操作说明](FakePod_使用操作说明.md)，视频手势见 [视频播放操作说明](视频播放操作说明.md)。

---

## 硬件平台

| 项目 | 规格 |
|---|---|
| SoC | ESP32-S3R8，双核 Xtensa LX7，240MHz |
| 内存 | 内置 SRAM + 8MB Octal PSRAM（80MHz） |
| Flash | 外置 16MB Quad Flash，DIO / 80MHz |
| 屏幕 | CO5300 AMOLED，460×460，QSPI，TE 同步 |
| 触摸 | CST820 电容触摸（I2C） |
| 姿态 | QMI8658 六轴 IMU（体感手势控歌） |
| 音频 | CS43131 DAC + I2S 输出 |
| 存储 | TF 卡（SDMMC / FATFS，UTF-8 长文件名） |
| USB | TinyUSB MSC（TF 卡）+ CDC（日志） |

---

## 目录结构

```text
.
├── CMakeLists.txt              # ESP-IDF 顶层工程
├── platformio.ini              # PlatformIO 环境定义
├── partitions.csv              # 16MB 分区表
├── sdkconfig.defaults          # 基础 sdkconfig 片段
├── src/
│   ├── main.cpp
│   ├── app/                    # APP 层：音乐/视频/电子书/NSF/图片/设置/启动器
│   ├── audio/                  # 音频服务、解码器、音源、频谱快照、NSF 合成
│   ├── assets/                 # 内嵌图片与动画帧
│   ├── board/                  # 板级引脚与屏幕尺寸定义
│   ├── boot/                   # 启动状态与版本输出
│   ├── drivers/                # i2c / touch / imu / storage / display / audio
│   ├── media/library/          # 曲库索引、元数据、封面、NAS 源
│   ├── player/                 # 播放列表、状态机、传输控制
│   ├── system/                 # 设置、持久化、BLE、WiFi、NAS、电源、锁屏
│   ├── text/                   # 文本编码
│   └── ui/                     # LVGL 界面、手势路由、歌词、频谱、控件
├── components/esp-dsp/         # 本地 vendored ESP-DSP
├── managed_components/         # 组件管理器下载的依赖
├── tools/                      # 电脑端工具（视频转换、NAS、固件合并）
└── tests/host/                 # 主机回归脚本
```

---

## 快速开始

### 环境准备

- Python 3.10+
- PlatformIO Core（建议 6.2.0 及以上，避免多版本共存）
- Git
- 工具链由 PlatformIO 自动安装（`toolchain-xtensa-esp-elf@14.2.0+20251107`）

### 编译

```powershell
pio run -e fakepod_nano
```

产物位于 `.pio/build/fakepod_nano/firmware.bin`。

### 烧录与监视

```powershell
pio run -e fakepod_nano -t upload
pio device monitor
```

串口默认按 `platformio.ini` 中的 `COM10` / `921600` 配置，按实际设备端口修改 `upload_port` 与 `monitor_port`。

`monitor_dtr = 0` 与 `monitor_rts = 0` 用于固定监视器打开时的 DTR/RTS 初始电平，降低打开/关闭日志端口触发 USB 复位的概率。

---

## 构建环境

| 环境 | 用途 |
|---|---|
| `fakepod_nano` | 默认正式固件，`APP_DIAG_PROFILE=0`（RELEASE） |
| `fakepod_perf` | 打开 FLAC 与频谱性能计时 |
| `fakepod_perf_portable` | 同上，但频谱改用可移植 FFT，便于做 A/B 对比 |
| `fakepod_nas_diag` | NAS 流诊断，不打开编解码全量日志 |
| `fakepod_local_diag` | 本地 PCM 喂数据 + 频谱计时的可视化诊断 |
| `fakepod_overlay` | 屏幕叠加 CPU0/CPU1 与 LVGL 实际帧率 |

关键配置：

- `board_build.esp-idf.sdkconfig_path = sdkconfig.fakepod_nano_usbcdc`（由 `sdkconfig.defaults;sdkconfig.ble.defaults` 生成）。
- `sdkconfig.defaults` 关键项：240MHz、PERF 优化、DIO/80MHz/16MB、8MB Octal PSRAM、`CONFIG_SPIRAM_USE_MALLOC`、AVI 之外的其他解封装器全部关闭、TinyUSB MSC+CDC、NimBLE 双角色。
- LVGL 使用内置 TLSF，但 128KB pool 强制落在 PSRAM（见 [src/CMakeLists.txt](src/CMakeLists.txt)）。
- `esp_lcd_panel_rgb.c` 单独使用 `-O0`，规避 Xtensa GCC 14.2.0 的 IRA 阶段编译崩溃；本项目实际走 QSPI CO5300，不使用 RGB 数据路径。

分区表（[partitions.csv](partitions.csv)）：

| 分区 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 20K |
| otadata | data/ota | 0xe000 | 8K |
| app0 | app/ota_0 | 0x10000 | 4M |
| app1 | app/ota_1 | 0x410000 | 4M |
| fontcache | data/0x40 | 0x810000 | 3M |
| spiffs | data/spiffs | 0xb10000 | 4992K |
| coredump | data/coredump | 0xff0000 | 64K |

---

## 版本基线

| 组件 | 版本 | 固定方式 |
|---|---|---|
| PlatformIO 平台 | `espressif32 6.13.0` | `platform = espressif32 @ 6.13.0` |
| ESP-IDF | `5.5.3` | 随平台版本间接锁定 |
| Xtensa GCC | `14.2.0+20251107` | `platform_packages` |
| 托管组件（LVGL 9.2.2、esp_extractor 等） | 见 [dependencies.lock](dependencies.lock) | 锁文件入库 |
| PlatformIO Core | 建议 `6.2.0` 及以上 | 文档约定 |
| Python | `3.10+`（本机验证 3.13） | 文档约定 |

说明：

- 平台、工具链与组件版本均已固定，换机器时只需安装匹配的 PlatformIO Core 与 Python，即可复现同一构建。
- `managed_components/` 被 `.gitignore` 忽略，组件由 `dependencies.lock` 复现，不要手工改动其中的版本。
- PlatformIO Core 只保留一个安装：多版本共存会触发 `Obsolete PIO Core` 警告，并可能在 `C:\PlatformIO\platforms.lock` 上出现 `PermissionError`。
- `sdkconfig.*` 生成物同样不入库，由 `sdkconfig.defaults` 重新生成，无需固定。

---

## 完整固件打包与离线烧录

1. 先完成一次工程编译，确保 `.pio/build/<env>/` 内有 bootloader、分区表与应用镜像。
2. 运行 [tools/firmware/merge_fakepod_full.bat](tools/firmware/merge_fakepod_full.bat) 合并为单一完整固件。
3. 使用乐鑫 `flash_download_tool` 离线烧录：

| 项目 | 设置 |
|---|---|
| 文件 | 合并后的完整固件（如 `FakePod_1.4.0_FULL_16MB.bin`） |
| 地址 | `0x0000` |
| SPI MODE | DIO |
| SPI SPEED | 80MHz |
| Flash Size | 16MB |

> `*.bin` 已被 `.gitignore` 忽略，发布产物不入库。

---

## TF 卡目录约定

| 内容 | 路径 | 说明 |
|---|---|---|
| 音乐 | `/MUSIC` | 最多二级文件夹；MP3 / FLAC / WAV / Opus |
| 视频 | `/video` | AVI，可放子文件夹；建议同时放置同名 `.fvix` 索引 |
| 电子书 | `/txt` | UTF-8 编码，可使用子文件夹 |
| 电子音流 | `/synth` | NSF；NSFE 可识别但暂不能正常播放 |
| 图片 | SD 卡任意位置 | 图片浏览会扫描 `/sdcard` 下的 JPG / PNG |

标签与封面可通过 MusicTag 写入，歌词文件建议用 LDDC 下载。也可在设备内通过「设置 → 连接 → TF 卡文件管理」用电脑直接整理。

---

## 视频子系统

数据流：

```text
AVI 文件 → esp_extractor（仅保留 AVI）→ MJPEG 帧 → RGB565
        → BoundedSPI Presenter（QSPI 直发 CO5300，独占 SPI2 DMA，绕过 LVGL）
```

- 视频播放期间 LVGL 界面整体隐藏，UI 元素由 Presenter 的 producer 回调直接合成到 RGB565 缓冲。因此 BoundedSPI session 与 LVGL flush 互斥，一次播放结束后必须等 pipeline 完全退出才能开启下一个 session。
- 音频轨由 AVI 内 MP3 流经 `avi_mp3_audio_source` 接入统一音频服务；视频暂停时音频与画面同步暂停。
- 手势：右滑快进、左滑后退（速度由起手位置在上下半屏决定）、下滑返回列表、原地双击暂停/继续，详见 [视频播放操作说明](视频播放操作说明.md)。

### FVIX 侧车索引

- 使用 [tools/video/FFmpeg_MJPEG_GUI_v4](tools/video/FFmpeg_MJPEG_GUI_v4/README.md) 转换视频时，会为每个 AVI 自动生成同名 `.fvix` 索引，需与 AVI 一起拷入设备。
- 索引每 2 秒记录一个定位点，文件很小；没有 `.fvix` 的视频仍可正常播放，但**不能 seek**。
- 定位为粗精度：落点是不晚于目标时间的下一帧，实测与滑动估算可能相差 1~2 秒。
- 索引格式为 FVIX V2，包含来源文件校验，拒绝与当前视频不匹配的索引。

---

## 工具

| 用途 | 启动位置 | 说明 |
|---|---|---|
| 转换视频 | [tools/video/FFmpeg_MJPEG_GUI_v4/start.bat](tools/video/FFmpeg_MJPEG_GUI_v4/start.bat) | 批量转 460 宽 MJPEG AVI，并生成 `.fvix` |
| 管理 NAS 曲库 | [tools/nas/启动NAS曲库管理器.bat](tools/nas/启动NAS曲库管理器.bat) | 见 [NAS 工具说明](tools/nas/FakePod_NAS_GUI_使用说明.md) |
| 合并完整固件 | [tools/firmware/merge_fakepod_full.bat](tools/firmware/merge_fakepod_full.bat) | 需先完成工程编译 |

工具总览见 [tools/README.md](tools/README.md)。

---

## 主机回归测试

`tests/host/` 下是维护源码时使用的主机侧脚本，不在 ESP32 上运行，也不参与固件编译：

```powershell
python tests/host/test_independent_browse.py
python tests/host/test_nas_exclusive_restore.py
python tests/host/test_audio_perf_math.py   # 需要 g++
```

这些脚本使用主机替身，不能替代完整固件构建与真机验收。清单见 [tests/host/README.md](tests/host/README.md)。

---

## 开发约定

- **提交信息使用中文**，遵循 Conventional Commits：`<type>(<scope>): <中文描述>`，scope 用小写英文模块名（如 `video`、`display`、`audio`）。规则见 [.trae/rules/git-commit-message.md](.trae/rules/git-commit-message.md)。
- **版本号只在 [src/system/app_build_info.h](src/system/app_build_info.h) 维护**，运行期由 Boot 层统一输出一次。
- **改动视频 / 音频 / 显示通路时同步更新文档**：操作行为变化更新 `FakePod_使用操作说明.md` 或 `视频播放操作说明.md`。
- 诊断开关统一走 `APP_DIAG_PROFILE` 与 `APP_DIAG_*` 宏，正式固件保持 RELEASE。

---

## 常见问题

| 现象 | 原因与处理 |
|---|---|
| CMake 报 `SDKCONFIG_DEFAULTS 'sdkconfig.ble.defaults' does not exist` | 该文件被 `.gitignore` 的 `sdkconfig.*` 规则忽略，不在版本库中。首次克隆需自行补齐该文件，或删除 `board_build.cmake_extra_args` 中的对应项后重新配置。 |
| `PermissionError: 'C:\PlatformIO\platforms.lock'` | 系统存在多个 PlatformIO Core（提示 obsolete PIO Core），或对 `C:\PlatformIO` 无写权限。清理多余安装、统一 Core 版本后重试。 |
| 烧录成功但串口反复复位 | 检查 `monitor_dtr` / `monitor_rts` 是否为 0；USB CDC 与 UART 的串口号可能不同。 |
| 视频无法快进/快退 | 缺少同名 `.fvix` 索引，用视频转换工具重新生成。 |
| 改动设置后未生效 | 「设置 → 应用 → 音乐播放器」中的部分选项需**离开整个设置 APP** 后才正式保存生效。 |
| `fakepod_overlay` 环境配置失败 | 该环境额外引用 `sdkconfig.overlay.defaults`，需本地自备该文件。 |

---

## 相关文档

- [FakePod 使用操作说明](FakePod_使用操作说明.md) —— 面向使用者的完整操作手册
- [视频播放操作说明](视频播放操作说明.md) —— 视频手势、SEEK 精度与片源要求
- [tools/README.md](tools/README.md) —— 电脑端工具入口
- [tools/video/FFmpeg_MJPEG_GUI_v4/README.md](tools/video/FFmpeg_MJPEG_GUI_v4/README.md) —— 视频转换与索引生成
- [tests/host/README.md](tests/host/README.md) —— 主机回归脚本清单
- [.trae/rules/git-commit-message.md](.trae/rules/git-commit-message.md) —— 提交信息规范

---

## 第三方组件

固件基于 ESP-IDF 构建，依赖并以 `managed_components/` 形式引入：LVGL 9.x、esp_extractor、esp_lv_decoder、esp_new_jpeg、libpng、esp_lcd_co5300、esp_lvgl_port、esp_audio_codec、esp_tinyusb、cmake_utilities，另有本地 vendored 的 ESP-DSP。各组件的许可证与版权声明见对应目录内的 `README.md` / `LICENSE`。
