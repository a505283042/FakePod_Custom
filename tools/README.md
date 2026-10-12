# FakePod 辅助工具（分类目录）

按用途整理；仅移动工具和更新其相对路径，**不影响 ESP32 固件编译和功能**。

| 目录 | 功能 | 常用入口 |
|---|---|---|
| [`nas/`](nas/) | NAS V2 索引生成、WebDAV/SSH 管理、封面定位、配置样例、使用说明 | 双击 [`启动NAS曲库管理器.bat`](启动NAS曲库管理器.bat) |
| [`video/`](video/) | FFmpeg 视频转 AVI/MJPEG/MP3 | [`video/FFmpeg_MJPEG_GUI_v4/start.bat`](video/FFmpeg_MJPEG_GUI_v4/start.bat) |
| [`ui_assets/`](ui_assets/) | 生成 Launcher 动画资源 | `python tools/ui_assets/generate_launcher_animation_frames.py` |
| [`firmware/`](firmware/) | PlatformIO 固件完整镜像合并 | 双击 [`merge_fakepod_full.bat`](merge_fakepod_full.bat) |
| [`diagnostics/`](diagnostics/) | 源码与显示边界检查（只读） | `python tools/diagnostics/audit_source_inventory.py` |

## NAS 管理

Windows 上依旧可以双击 `tools/启动NAS曲库管理器.bat`；它会转到 `tools/nas/` 里的管理器。

命令行生成索引：

```powershell
python tools/nas/build_nas_catalog_v2.py --help
```

为了兼容旧命令，`tools/build_nas_catalog_v2.py` 保留为简短转发入口；实际实现及
`nas_artwork_locator_v2.py` 均位于 `nas/`。NAS 管理器内部调用同目录生成器，
既有扫描缓存、SSH Prepare、V2 发布协议和生成文件名均未改变。

## 固件合并

旧的 `tools/merge_fakepod_full.bat` 仍然可双击使用；实际脚本移到
`tools/firmware/`，默认仍在**项目根目录**读取 `.pio/build/<env>` 与
`partitions.csv`，并将 `FakePod_Full.bin` 写到项目根目录。

## 说明

- `tools/` 根目录只保留 README 与旧入口的兼容转发器，实际工具已分类。
- 工具的**工作数据、配置及 NAS 用户账号/密码不要放进源码包**。
- `video/` GUI 的 `start.bat` 与 `app.ps1` 保持同目录，运行逻辑不变。
