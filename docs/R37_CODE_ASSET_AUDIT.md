# FakePod Custom — R.37.0 代码资产盘点

> 基线：P1.5.3.2R.36.6.2  \n> 目的：在任何删除/启动重构前，先固定当前代码资产、日志热点与 Legacy 候选。

## 1. 基线规模

- `src/` 文件总数（含资源/配置）：108
- C/C++ 代码与头文件：106
- C/C++ 总行数：约 **44,164**
- `ESP_LOG*` 调用：约 **646**

### 按一级模块

| 模块 | C/C++文件 | LOC | ESP_LOG* |
|---|---:|---:|---:|
| `(root)` | 1 | 21 | 0 |
| `assets` | 4 | 4,899 | 10 |
| `audio` | 22 | 8,732 | 173 |
| `board` | 1 | 87 | 0 |
| `boot` | 2 | 551 | 32 |
| `drivers` | 17 | 5,138 | 164 |
| `media` | 17 | 8,250 | 63 |
| `player` | 8 | 1,509 | 33 |
| `system` | 4 | 573 | 7 |
| `ui` | 30 | 14,404 | 164 |

## 2. 日志热点

| 文件 | LOC | ESP_LOG* |
|---|---:|---:|
| `src/audio/audio_service.cpp` | 2,327 | 64 |
| `src/audio/decoders/flac_decoder.cpp` | 2,328 | 54 |
| `src/drivers/display/display.cpp` | 1,745 | 52 |
| `src/drivers/audio/cs43131.cpp` | 677 | 42 |
| `src/media/library/media_library.cpp` | 1,062 | 31 |
| `src/boot/boot_state.cpp` | 515 | 32 |
| `src/ui/screens/player_home.cpp` | 3,784 | 53 |
| `src/ui/ui_manager.cpp` | 1,020 | 32 |
| `src/audio/decoders/mp3_decoder.cpp` | 1,299 | 23 |
| `src/ui/font/font_manager.cpp` | 444 | 20 |
| `src/player/player_transport.cpp` | 428 | 19 |
| `src/drivers/audio/i2s_output.c` | 432 | 18 |
| `src/audio/decoders/wav_decoder.cpp` | 361 | 17 |
| `src/drivers/storage/sdcard.cpp` | 352 | 16 |
| `src/ui/screens/library_view.cpp` | 2,179 | 18 |

## 3. 当前正式路径（KEEP）

- `AudioTask` 单一音频所有者、命令队列、Snapshot、playback revision。
- FLAC Adaptive Prefetch / 192KB ring（<=96k 当前验证配置）。
- CoverSurface current-only：`normal + dimmed` 双 RGB565 Surface。
- Cover：R.36.4 BoundedSPI。
- Launcher：R.36.6.2 wire-order strip compositor，`FullBase=0B / PanelWork=0B`。
- Launcher 动态封面 lease rebind。
- R.32 LVGL Launcher fallback。
- LVGL 官方 flush 路径（歌词/频谱/曲库/HomeResume）。
- Catalog V2 / transaction store / storage mutex。

这些路径在 R.37.0/R.37.1 不做算法重写。

## 4. 安全回退（FALLBACK，禁止误删）

- Launcher R.32 LVGL 实时圆弧 fallback。
- Artwork LVGL 压缩图 fallback（progressive/不兼容图片）。
- Display BoundedSPI timeout/recovery/controlled restart。
- PresentHold 兼容回退。
- Audio Fault Snapshot。
- TE timeout 自动降级。

原则：平时没有调用不等于 Legacy。只要承担故障恢复职责，就必须保留。

## 5. 明确 Legacy 候选（R.37.2 再删）

### 5.1 `display_present_rgb565_direct()` / R.29 ContinuousGRAM — R.37.2 已删除

R.37.2 已删除旧 PanelIO DirectPresent 的公开 API、统计结构与 `display.cpp` 实现。
Cover/Launcher 继续使用 BoundedSPI；HomeResume 固定走 LVGL 官方 flush。审计脚本现将旧 Direct symbol 视为禁止重新引入。

### 5.2 `kLauncherDirectSceneEnabled=false` 历史分支 — R.37.2 已删除

固定 false 的历史常量已删除；现行 Launcher 高速状态仍由 BoundedSPI session 状态管理。
同批删除 `kFullscreenHomeResumeDirectEnabled=false` 与不可达 HomeResume Direct helper。

### 5.3 源码中的阶段号历史注释

大量 `P1.x / R.xx / Stage xx` 注释已经成为开发日志。后续只保留“为什么这样设计”的原因注释；版本历史交给 Git tag/commit。

## 6. R.37.1 日志策略

诊断统一为三个 Profile：

```text
RELEASE = 0
DEBUG   = 1
STRESS  = 2
```

RELEASE：
- 保留 `ESP_LOGW/E`；
- 保留启动版本 + READY 摘要；
- 关闭 Launcher 逐帧性能；
- 关闭 BoundedSPI 成功 Session Trace；
- 关闭 LVGL R.30/R.23 性能审计；
- 关闭普通 UI 点击/手势 Trace。

DEBUG：
- 增加启动细节；
- 增加 UI 交互 Trace。

STRESS：
- 打开 UI 性能审计；
- Launcher frame/perf；
- Display Transport；
- 既有 Audio/Library/Artwork 专项诊断。

任一 `APP_DIAG_xxx` 仍允许在 PlatformIO 单独覆盖。

## 7. 后续顺序

```text
R.37.0 资产盘点                 ← 本文
R.37.1 日志架构收口             ← 当前代码阶段
R.37.2 Legacy / dead code 删除
R.37.3 Public API slimming
R.37.4 Boot Orchestrator 重建
R.37.5 BootResult / Fatal-Degraded
R.37.6 READY boundary
R.37.7 RELEASE/DEBUG/STRESS 收口
R.37.8 CMake/include/warning hygiene
```

R.36.6.2 tag 继续作为 R.37 重构前黄金回退点。
