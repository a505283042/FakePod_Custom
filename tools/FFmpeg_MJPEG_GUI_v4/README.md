# FFmpeg MJPEG GUI v4 — Windows PowerShell 5.1 兼容修正版

本版针对你实际报告的两个问题做了修复：

1. PowerShell 5.1 会把中文弯引号 `“ ”` 当作字符串引号处理，导致脚本解析失败。
   - v4 已彻底移除脚本中的弯引号。
2. `ffmpeg.exe` 能找到，但 `ffprobe.exe` 没有加入 PATH。
   - v4 会自动在 `ffmpeg.exe` 所在目录寻找 `ffprobe.exe`。

## 启动

完整解压后双击：

`start.bat`

如果仍然出现错误，运行：

`diagnose.bat`

并把窗口内容发给 ChatGPT。

## 默认转换参数

- AVI
- MJPEG
- 宽度 460
- 24 FPS
- MJPEG q=8
- MP3 192 kbps
- yuvj420p
- 可自动检测黑边
