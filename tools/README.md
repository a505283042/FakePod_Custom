# FakePod 工具

日常使用按下表打开对应工具。

| 用途 | 启动位置 | 操作说明 |
|---|---|---|
| 电脑管理 NAS 曲库 | [nas/启动NAS曲库管理器.bat](nas/启动NAS曲库管理器.bat) | [NAS 工具说明](nas/FakePod_NAS_GUI_使用说明.md) |
| 转换视频 | [video/FFmpeg_MJPEG_GUI_v4/start.bat](video/FFmpeg_MJPEG_GUI_v4/start.bat) | [视频转换说明](video/FFmpeg_MJPEG_GUI_v4/README.md) |
| 合并完整固件 | [firmware/merge_fakepod_full.bat](firmware/merge_fakepod_full.bat) | 需要先完成工程编译，再运行合并工具 |

播放器、手机遥控及 NAS 首次配置，查看项目根目录的 [FakePod 使用操作说明](../FakePod_使用操作说明.md)。

`ui_assets/` 是界面素材生成工具，普通使用无需运行。

维护回归测试已移到 [tests/host](../tests/host/README.md)，不再放在日常工具目录，也不参与固件运行。

扫描工作数据、NAS 登录资料和生成的缓存不应打进源码包。
