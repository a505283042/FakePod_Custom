# FakePod 辅助工具（分类目录）

按用途整理；仅移动工具和更新其相对路径，**不影响 ESP32 固件编译和功能**。

| 目录 | 功能 | 常用入口 |
|---|---|---|
| [`nas/`](nas/) | NAS V2 索引生成、WebDAV/SSH 管理、封面定位、配置样例、使用说明 | 双击 [`nas/启动NAS曲库管理器.bat`](nas/启动NAS曲库管理器.bat) |
| [`video/`](video/) | FFmpeg 视频转 AVI/MJPEG/MP3 | [`video/FFmpeg_MJPEG_GUI_v4/start.bat`](video/FFmpeg_MJPEG_GUI_v4/start.bat) |
| [`ui_assets/`](ui_assets/) | 生成 Launcher 动画资源 | `python tools/ui_assets/generate_launcher_animation_frames.py` |
| [`firmware/`](firmware/) | PlatformIO 固件完整镜像合并 | 双击 [`firmware/merge_fakepod_full.bat`](firmware/merge_fakepod_full.bat) |
| [`diagnostics/`](diagnostics/) | 源码与显示边界检查（只读） | `python tools/diagnostics/audit_source_inventory.py` |

## NAS 管理

Windows 上双击 `tools/nas/启动NAS曲库管理器.bat` 即可打开 NAS 曲库管理器。

命令行生成索引：

```powershell
python tools/nas/build_nas_catalog_v2.py --help
```

索引生成器和 `nas_artwork_locator_v2.py` 都位于 `nas/`。NAS 管理器仍调用
同目录生成器，既有扫描缓存、SSH Prepare、V2 发布协议和生成文件名均未改变。

## 固件合并

双击 `tools/firmware/merge_fakepod_full.bat`，默认仍在**项目根目录**读取 `.pio/build/<env>` 与
`partitions.csv`，并将 `FakePod_Full.bin` 写到项目根目录。

## 说明

- `tools/` 根目录只保留 `README.md`，所有工具直接从对应分类目录启动。
- 工具的**工作数据、配置及 NAS 用户账号/密码不要放进源码包**。
- `video/` GUI 的 `start.bat` 与 `app.ps1` 保持同目录，运行逻辑不变。

## 目录清理说明

这里只保留仍在使用的脚本、启动入口和当前 NAS 说明。旧版 HTTP 8080 /
`/web/music-index` 配置样例与历史部署笔记已移出当前工具目录，避免误用。
如需回退旧版部署，请查阅旧版本源码包，不要直接套用过去的 URL。

- `nas/FakePod_NAS_GUI_使用说明.md`：当前 NAS 配置与①/②/③操作说明。
- 已移除 `tools/` 根目录三个旧兼容入口；旧命令需要改用各分类目录中的真实路径。
- 真实的 `.pio/` 构建文件、缓存、扫描工作数据与凭据不属于源码；
  本轮不会在用户电脑上执行清理，更不会删除 Music 或 `_FakePod`。
