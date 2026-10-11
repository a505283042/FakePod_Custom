# FakePod NAS V2 Windows 曲库管理器 — 增量更新版

基线：R46.0.119 固件 + 上一版 WebDAV 图形化 Windows 工具。**固件不需要改动或重烧**。

## 一、只选择一次 Music

1. Windows 11 安装 Python 3（带 Tkinter），双击 `tools/启动NAS曲库管理器.bat`。
2. 填入 NAS WebDAV 地址、端口、用户名和密码，在 WebDAV 中浏览到 `/麦田广告/Music`，选择「使用当前目录作为 Music」。
3. 自动推导同一个 Music 下的 `/Music/_FakePod/music-index` 和 `/Music/_FakePod/track`。GUI 通过 SMB 读取和发布文件，NAS 端通过 SSH 建立硬链接。无需分别选择三个路径。
4. Windows 文件资源管理器要能访问 `\\NAS地址\麦田广告\Music`。NAS 必须将 WebDAV、SMB、SSH 指向同一份 Music；默认 SSH 卷位置 `/volume1`，其他卷在高级选项调整。

## 二、增量工作原理

- 首次无 `music-index/fakepod_scan_cache_v1.json` 时，会完整读取元数据并建立缓存。**这一次叫“首次建立基线”，不是把 4227 首误报为新增 4227 首。**
- 下次扫描仅遍历目录和查看文件的 `size + mtime_ns`；未变化曲目直接复用标题、歌手、专辑、年代等缓存。**新增、修改**才读取音频标签，**删除**从新索引排除。
- GUI 显示总数、增加/修改/删除/复用数和变化样例。完整 Index 和 Manifest **仍保持原 V2 二进制格式**，ESP32 R46.0.119 直接用已有开机/手动同步检测新 revision。
- 极端情况下如果文件内容改变，但文件大小和修改时间被精确保留，增量模式不能识别；可勾选「重建全部元数据缓存」强制扫描（强制扫描不会提供可靠的增量删除列表）。

## 三、执行顺序（重要）

先在手机或设备上**停止 NAS 音乐播放**。

1. 点「扫描并预览增量更新」，确认变更数量无误。此时仅生成到 Windows 临时目录，NAS 原索引和音乐没有改动。
2. 如果涉及新增/修改的 MP3/FLAC 或有要隔离的旧标准短 ID，点 **① SSH 准备新增/修改短 ID**；Windows 弹出 SSH 终端，需要输入密码。看到 `FakePod incremental prepare OK` 才算成功。该步骤在 NAS 内部使用 `ln`，不会复制音频；对于已存在且指向同一 inode 的链接，不重写。
3. 点 **② 验证后发布 V2 索引**。GUI 会读取 NAS `music-index` 的对应**本轮准备成功标记**，验证脚本一致，且复核歌曲大小/修改时间/路径未变化，再备份旧索引、发布新索引及扫描缓存；`fakepod_catalog.meta` 最后提交。标记缺失会阻止发布。
4. 如果删除了曲目，可选择点 **③ SSH 隔离删除的旧 ID**。会将缓存明确识别的被删除曲目的**原标准短 ID**移动到 `Music/_FakePod/quarantine-<批次>`；不永久删除文件，不触碰额外旧兼容别名。需要时可从隔离目录恢复硬链接。**先发布索引，再隔离旧 ID。**
5. 如果本轮没有任何短 ID 变化，可以直接执行步骤 ②。
6. ESP32 下次开机自动检查 NAS 索引；设备「设置 → 连接 → NAS更新」也可手动同步。更新成功后旧 PSRAM 曲库仍可能驻留，退出曲库并真正释放后再打开，或重启以查看新增曲目。

## 四、保护和限制

- 继续保留旧 `fakepod_make_static_track_links.sh` 以维持 V2 Meta 的脚本校验兼容，但 **GUI 不再执行整目录重建脚本**，只运行 `fakepod_incremental_update.sh`。
- 原音乐始终不删除、不搬动；额外手工修复的 **23 个兼容短 ID** 不会自动隔离或清理。
- 「隔离删除」只识别**上次已发布的扫描缓存内**对应的旧标准短 ID。第一次建立基线不会贸然删除旧链接。
- 发布使用 `_FakePod/backup-index-*` 保存上一套索引/缓存；取消发布不会修改原索引。
- `stat`、`ln`、`mv` 和 `/bin/sh` 必须在 NAS 上可用（Synology DSM 5.x / BusyBox 常见）。如果 SSH 终端运行失败，不应继续发布。
- HTTP WebDAV 5005 不加密，请只在可信局域网中使用；对公网建议另行实现 HTTPS/TLS。
- 如果 SMB 上不能读取 `_FakePod/music-index` 或没有写权限，无法安全发布。切勿用 Windows 普通复制代替 NAS 硬链接。
- **不保证无缝热更新**；建议暂停 NAS 播放并结束其他 NAS 曲库维护操作后执行。

## 五、可选命令行

```powershell
python .\tools\build_nas_catalog_v2.py "\\192.168.1.105\麦田广告\Music" "C:\FakePod_Stage" --cache-from "\\192.168.1.105\麦田广告\Music\_FakePod\music-index\fakepod_scan_cache_v1.json"
```

命令仅在 `C:\FakePod_Stage` 创建**待发布**索引和报告，不直接修改 NAS。建议用 GUI 执行后续准备、验证、发布和隔离，以防操作顺序错误。
