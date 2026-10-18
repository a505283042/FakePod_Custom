# 主机检查与回归测试

这里是维护源码时使用的回归脚本，原位于 `tools/diagnostics/`。
这些文件不在 ESP32 上运行，正常听歌、使用 NAS 或编译固件时无需执行。

| 脚本 | 检查内容 |
|---|---|
| test_independent_browse.py | 独立浏览播放队列、切歌、自然 EOF、本地位置保存 |
| test_nas_exclusive_restore.py | NAS 从 NSF／视频返回音乐的恢复流程 |
| test_library_highlight_focus.py | 曲库高亮和定位 |
| test_nas_folder_count.py | NAS 目录计数 |
| test_nas_server_playback_bind.py | NAS 服务器切换后的播放绑定 |
| test_remote_power.py | BLE 状态发送节流 |

从项目根目录运行，例如：

```text
python tests/host/test_independent_browse.py
python tests/host/test_nas_exclusive_restore.py
```

需要 Python 3；执行 C++ 主机回归的脚本还需要 g++。这些测试使用主机替身，不能代替完整固件构建及真机验收。

旧的 R.37 源码清点脚本采用早期显示接口约束，已随历史资料单独存档，不作为当前版本验收标准。
