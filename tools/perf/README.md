# R147.6 音频与频谱测量

普通构建仍使用 `fakepod_nano`，详细性能统计默认关闭。

在 VS Code 的 PlatformIO 终端执行（上传端口沿用现有配置）：

```powershell
pio run -e fakepod_perf -t upload
pio device monitor -b 115200
```

`fakepod_perf` 使用 S3 DSP FFT，并开启既有 FLAC 指标和新增频谱统计。日志每约 5 秒汇总一次。

需要做 FFT 对照时，用同样的歌曲、音量、页面和网络环境改烧：

```powershell
pio run -e fakepod_perf_portable -t upload
```

启动频谱后确认日志 backend，比较 `SPECTRUM_PERF` 的 avg/max、丢帧和栈余量；它统计窗函数、FFT、分频段及等级计算总耗时，不是屏幕刷新率。

同一首高采样率 FLAC 分别运行息屏、封面、频谱、磁带各约 1 分钟；本地和 NAS 分开测。保留 `FLAC_PERF_CONTEXT`、`PERF_TRACE`、欠载恢复和 `SPECTRUM_PERF` 日志，并说明听到断音的时间。

- `decode max`：单次解码峰值；`refill max`：补充解码输出的峰值。
- `ring/min`：压缩数据缓冲水位。NAS 使用 HTTP ring；NAS 的本地 fread/copy 统计不适用。
- `DMA_full_capacity` 是按配置计算的满队列理论容量，不是实时剩余音频时间，不是实测欠载次数。
- 缓冲充足而解码峰值大，考虑计算/调度；缓冲见底，考虑 TF/NAS 供给。需要结合时间相关性判断。

测量后恢复普通构建：

```powershell
pio run -e fakepod_nano -t upload
```

两种测试构建共享原 sdkconfig 路径，请顺序构建、上传，不并行运行。首次编译需要联网下载固定版本 ESP-DSP 1.6.0；组件管理器会更新 dependencies.lock，不要手改库哈希。
