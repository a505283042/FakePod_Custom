# R46.0.139：NAS 设置提交后同步播放队列

基于 R46138_NAS_Right228_SingleLine_ESP32_Source(1).zip。仅修改播放器固件，APP V0.5.25 可继续使用。

## 修正行为

发送 NAS 设置后，新索引下载并成功加载到 PSRAM，播放器将 NAS 全部歌曲队列绑定到第一首。返回播放页显示 NAS 当前歌曲，按播放键才开始播放。异步加载和命中驻留索引两条路径均处理。

普通浏览 NAS 不会把当前本地播放强制切走。同一 NAS 的普通索引更新继续走现有路径恢复歌曲及列表上下文，不触发首次选曲。不承诺新增音频时间点恢复功能。

加载失败不会绑定未就绪的 NAS；之后手动重新打开 NAS 列表成功时仍可补上绑定。若用户在加载期间切回本地，或主动开始其他播放，保留用户的新操作。

## 应用补丁

在原有 R46.0.138 项目上应用，不要重复应用旧版本补丁。PowerShell：

```powershell
cd "D:\ESP32\FakePod_Custom\FakePod_Custom"
git apply --check "C:\Users\YCB\Downloads\R46139_NAS_Playback_Queue_Bind.patch"
git apply "C:\Users\YCB\Downloads\R46139_NAS_Playback_Queue_Bind.patch"
pio run -e fakepod_nano
pio run -e fakepod_nano -t upload
```

也可在 VS Code 中使用原来的 PlatformIO 编译、上传按钮。完整源码包用于新目录打开；不覆盖原项目的本机配置。

## 设备验收

1. 当前选中本地第一首，从 APP 发送 NAS 设置，等待下载和 NAS 列表加载完成。
2. 返回播放页，应显示 NAS 第一首，保持停止状态；按播放键应播放该 NAS 曲目。
3. 在 NAS 中选中其他歌曲，再做同一 NAS 的普通曲库更新；检查恢复到仍存在的原歌曲及列表。
4. 主动播放本地歌曲时仅下拉浏览 NAS，不能静默更换当前播放源。
5. 启动日志应显示 R46.0.139；成功绑定日志为“新NAS播放队列已绑定：首曲已选中，不自动播放”。

## 验证范围

- 生产绑定函数的主机 C++ 状态回归测试通过：一次性绑定、未就绪/空库、加载重试、来源改回本地、已有 NAS 选择、播放中/暂停/打开/Seek 时保留新操作、绑定失败重试。
- 补丁在原 R46.0.138 源码包上试应用，并逐文件核对通过；完整源码 ZIP 校验通过。
- 此环境未安装 PlatformIO/ESP-IDF 工具链，未完成 ESP32 全量编译、RAM 统计或硬件实测。请以上面的编译和设备验证结果为准。
