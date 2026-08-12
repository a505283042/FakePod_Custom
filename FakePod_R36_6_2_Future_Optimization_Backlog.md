# FakePod Custom — R.36.6.2 后续优化方向记录

> 记录日期：2026-08-12  
> 当前正式显示基线：**P1.5.3.2R.36.6.2 — Wire-Order Strip Composer**  
> 目的：冻结当前稳定版本，并记录后续可选优化方向，后期按需要决定是否继续。

---

## 1. 当前 R.36.6.2 状态

当前 Launcher 显示链路已经完成：

```text
CoverSurface.dimmed
        ↓
Wire-Order Strip Compositor
        ↓
16行 × 2 INTERNAL DMA staging
        ↓
BoundedSPI
        ↓
CO5300
```

当前关键状态：

```text
FullBase                    = 0B
PanelWork                   = 0B
Launcher 专用 PSRAM         ≈ 0B
BoundedSPI                  PASS
Cover BoundedSPI            PASS
Launcher BoundedSPI         PASS
Launcher 动态封面换绑       PASS
PackBits Run Fast Path      PASS
Wire-Order Producer         PASS
RGB565 后处理 swap          0us
DMA staging 泄漏            0
显示永久卡死                0
```

R.36.6.2 已实现：

- Launcher 不再保留 460×460 FullBase。
- Launcher 不再保留 340×340 PanelWork。
- Launcher 直接复用 `CoverSurface.dimmed`。
- Launcher 打开期间自动切歌时，新封面 ready 后可原子换绑。
- PackBits I4 使用 run-level fast path。
- Strip compositor 直接生成 SPI wire-order RGB565。
- BoundedSPI 不再对 Launcher strip 做第二遍 byte swap。
- `swap=0us` 已经实机验证。
- Launcher 单帧典型约 15~20ms，复杂帧偶发约 20~29ms。
- 动画展开仍约 325~335ms，整体交互节奏正常。

---

# 2. 后续可优化 / 可修方向

## 2.1 R.36.6.3 — Launcher Raster Fast Path

**优先级：中**

### 目标

继续降低 Launcher `compose` 时间，重点优化复杂帧 `frame=8~11`。

### 当前剩余热点

- 中心圆逐像素几何判断。
- 中心图标逐像素 raster。
- 少量 AA / blend。
- strip 内几何分支。

### 可行方案

把圆形：

```text
dx² + dy²
```

逐像素判断，改成每帧预计算的行 span：

```text
frame N / y：
outer_left
outer_right
inner_left
inner_right
```

绘制时直接：

```text
fill outer span
fill inner span
```

### 预期收益

- 降低复杂帧 CPU 时间。
- 降低 frame=11 偶发 20~29ms 波动。
- 不改变 BoundedSPI / PSRAM 架构。

### 风险

低~中。

主要风险：

- 圆边缘视觉差异。
- AA 过渡可能出现轻微变化。

---

## 2.2 Launcher 图标 Raster Fast Path

**优先级：低~中**

### 目标

减少 Launcher 中心图标、小图形逐像素判断。

### 可行方向

预生成：

```text
每行 x_start / x_end
```

或 Flash mask。

运行时改成：

```text
连续 span fill
```

而不是逐像素 branch。

### 收益

预计小于 PackBits Fast Path，但可以进一步降低复杂帧 CPU 使用。

---

## 2.3 Launcher Strip 高度调优

**优先级：低**

当前：

```text
16 行 × 2 DMA staging
```

可后期实测：

```text
8 行
12 行
16 行
20 行
24 行
```

### 更大 strip

优点：

- transaction 数量更少。
- callback / queue 管理开销降低。

缺点：

- INTERNAL DMA 占用更高。
- producer 单次运行时间更长。
- Audio/UI 实时调度颗粒变粗。

### 当前建议

16 行已经稳定，不建议现在修改。

只应作为 benchmark 项。

---

## 2.4 Strip Compositor IRAM / Cache Locality

**优先级：中**

### 现象

同一 Launcher frame 在不同 Session 中仍有明显波动。

例如复杂帧可能出现：

```text
约 19ms
约 22ms
约 29ms
```

说明不仅是纯算法成本，还可能受：

- Core1 调度。
- PSRAM cache miss。
- Flash cache。
- 内存访问 locality。
- 其他 UI / Cover / Lyrics 任务竞争。

### 可优化方向

- 热函数 `inline`。
- 降低函数调用层级。
- decoder state / LUT 放 INTERNAL RAM。
- 避免随机访问 PSRAM。
- 热数据结构压缩。
- 必要时把极小关键函数放 IRAM。

### 建议

先做分段 profiling，再决定。

不要盲目加 IRAM 属性。

---

## 2.5 CoverSurface.dimmed 生成性能

**优先级：低**

当前 normal + dimmed 双 Surface 构建常见：

```text
约 200~300ms
```

主要在 Core1 后台完成。

### 后期方向

JPEG resize / sampling 时同时生成：

```text
normal
dimmed
```

避免第二遍完整采样。

### 价值

减少 CoverSurface 构建时间和 Core1 占用。

### 当前状态

不是交互阻塞项，优先级较低。

---

## 2.6 CoverSurface 双 Surface 内存进一步压缩

**优先级：中低**

当前每曲：

```text
normal  = 423,200B
dimmed  = 423,200B
总计    = 846,400B
```

当前 Launcher、Overlay 都能直接使用 dimmed，因此双 Surface 有实际价值。

### 极限方案

只保留：

```text
normal
```

然后：

```text
Launcher / Overlay 实时 dim
```

### 优点

最多再省约：

```text
423,200B PSRAM
```

### 缺点

- 实时 CPU 增加。
- strip compositor 更复杂。
- Overlay 路径可能重新引入全屏计算。

### 当前建议

**不推荐现在做。**

---

## 2.7 中文字体 Flash mmap

**优先级：高价值 / 独立阶段**

当前日志显示字体：

```text
约 1.66MB PSRAM
```

这是当前最大单项 PSRAM 占用之一。

### 长期方案

建立独立 Flash 字体分区：

```text
font partition
    ↓
esp_partition_mmap()
    ↓
LVGL / font loader 直接读取映射地址
```

### 预期收益

释放：

```text
约 1.6MB+ PSRAM
```

收益远高于继续优化 Launcher 几毫秒。

### 涉及

- partition.csv
- 字体资源烧录
- OTA 兼容策略
- 字体 loader
- mmap 生命周期

### 建议

作为独立大版本处理。

---

## 2.8 HomeResume 大刷新优化

**优先级：中**

当前：

```text
Spectrum → Home
Lyrics   → Home
Library  → Home
```

仍主要使用：

```text
LVGL
direct=0
```

### 后期方案

利用已有：

```text
CoverSurface.normal / dimmed
```

先通过 BoundedSPI 恢复物理整屏封面，再让 LVGL 只恢复必要控件。

### 目标

降低：

```text
460×460 LVGL full refresh
```

减少返回主页时：

- render
- flush
- TE 等待
- 大面积 invalidation

### 当前状态

功能稳定，不属于 bug。

---

## 2.9 旧 R.29 / Legacy Direct 清理

**优先级：中低**

当前运行时：

```text
Cover → R.36.4 BoundedSPI
Launcher → R.36.x BoundedSPI
```

旧：

```text
R.29 ContinuousGRAM
legacy PanelIO Direct
```

已不再作为主页主路径。

### 后期可以删除

- 无调用 Direct API。
- 旧统计结构。
- 旧 feature flag。
- 历史兼容分支。
- 无效日志。

### 收益

- 减少代码维护成本。
- 防止未来误启旧无界路径。
- Display 架构更清晰。

### 建议

等显示架构彻底冻结后统一清理。

---

## 2.10 BoundedSPI 对 ESP-IDF Private Layout 的依赖隔离

**优先级：中高 / 长期健壮性**

当前 R.36.3 BoundedSPI 为复用 Panel IO 的 SPI device，依赖特定 ESP-IDF 5.5 private layout/prefix。

当前已经有：

- ESP-IDF 版本检查。
- Runtime guard。
- failure fallback。
- bounded timeout。

所以当前版本可用且实机稳定。

### 长期方向

方案 A：

```text
独立创建 DisplayTransport 自己拥有的 SPI device
```

方案 B：

```text
把 ESP-IDF private layout 适配封装成 Compatibility Layer
```

### 当前建议

不要为了“架构纯洁”马上改。

当前 BoundedSPI 已经经过大量实机压力验证。

---

## 2.11 统一 DisplayTransport

**优先级：长期架构**

长期可形成：

```text
Cover
Launcher
HomeResume
Picture
EBook
Other fullscreen content
        ↓
DisplayTransport
        ↓
Bounded SPI owner
        ↓
CO5300
```

LVGL 负责：

```text
控件
文本
列表
普通 UI
```

DisplayTransport 负责：

```text
大面积 framebuffer
strip stream
full-screen present
```

### 价值

- 所有大面积显示路径统一。
- ownership 更清晰。
- 避免旧 PanelIO Direct 路径再次出现。
- 后续图片/电子书可复用。

### 当前建议

长期演进，不要求一次完成。

---

# 3. Audio 侧后续项

## 3.1 FLAC EOF 尾帧告警

**优先级：中 / 独立 Audio 阶段**

曾实机出现：

```text
AUD_Dec_Parse: Not found frame in max frame size ...
```

随后仍正常：

```text
FLAC 播放完成
自动下一曲
```

说明当前不是功能故障。

### 后期检查

- EOF 最后一块输入边界。
- decoder 是否在已知 EOF 后多 parse 一次。
- truncated tail 判断。
- EOF flush 顺序。
- decoder return code 与真实 EOF 的区分。

### 建议

不要与显示优化混在一个版本里。

---

## 3.2 MP3 INDEX_TRACE total=0 告警

**优先级：低**

当前 MP3 初始化时可能出现：

```text
index total != decoder total=0
```

通常是 decoder 刚打开时 total 尚未知。

### 后期改进

如果：

```text
decoder total == unknown / 0
```

则：

```text
不立即输出 MISMATCH
```

等 metadata/decoder total 有效后再比较。

### 性质

日志质量问题，不影响播放。

---

## 3.3 Artwork FLAC QoS 数据化

**优先级：低**

当前已经实机验证：

```text
<90%    pause
90~91%  2KB
92~95%  4KB
>=96%   8KB
```

策略正常。

### 后期可增加统计

```text
pause 次数
2KB 命中次数
4KB 命中次数
8KB 命中次数
累计等待时间
平均 Artwork 完成时间
```

再根据真实长期数据决定是否调整阈值。

当前无需修改。

---

# 4. 高采样率验证

## 4.1 96k / 192k FLAC 专项压力测试

**优先级：高价值测试项**

96k 已有一定覆盖。

192k 仍建议单独测试。

### 推荐组合

```text
192k / 24bit FLAC
+
Launcher 连续开关
+
Cover 切歌
+
Lyrics
+
Spectrum / FFT
+
Library
```

### 重点记录

```text
FLAC ring min
Emergency count
AUDIO_FAULT
DMAfree
largest DMA
Launcher frame total
compose
stale_total
BoundedSPI fault
```

### 目的

确认高采样率音频负载下，当前 zero-PSRAM Launcher compositor 是否仍有足够 Core1 / DMA 余量。

---

# 5. 正式版本日志降噪

## 5.1 Launcher Perf Log Gate

**优先级：低 / 封版前**

当前为了性能调试保留：

```text
compose
stream
wait
pairs(skip/fill/lit/blend)
seq
chunks
staging
PanelWork
```

架构稳定后建议统一放入：

```text
APP_DIAG_LAUNCHER_PERF
```

正式版本默认关闭。

仅保留：

```text
Session BEGIN/END
fault
fallback
controlled restart
fatal descriptor mismatch
```

### 价值

减少：

- UART 日志本身对实时调度的影响。
- 正式版本日志噪音。
- 性能测量被串口输出污染。

---

# 6. 推荐未来处理顺序

如果后期重新开始优化，建议优先顺序：

```text
1. 192k FLAC 专项压力验证
2. 中文字体 Flash mmap
3. R.36.6.3 Launcher Raster Fast Path
4. HomeResume Bounded Present
5. FLAC EOF 尾帧告警收口
6. Legacy Direct / R.29 代码清理
7. BoundedSPI private-layout compatibility layer
8. DisplayTransport 长期统一
```

其中：

```text
R.36.6.2 当前建议：冻结
```

不建议为了追求少量毫秒收益继续频繁修改已经稳定的显示传输链。

---

# 7. 当前冻结结论

```text
版本：P1.5.3.2R.36.6.2

显示永久卡死问题              基本收口
Launcher BoundedSPI           稳定
Cover BoundedSPI              稳定
Launcher FullBase             0B
Launcher PanelWork            0B
Launcher 专用 PSRAM           ≈0B
Wire-order producer           PASS
RGB565 post swap              0us
动态封面换绑                 PASS
DMA staging leak              0
FLAC Artwork QoS              PASS
```

当前最适合作为下一阶段开发基线。
