# FakePod R146 + Android V0.5.34：APP 曲库浏览与选歌

本版是新功能验收候选版。基线为 ESP32 R145.23.1、Android V0.5.33。
R145.23.1 稳定基线保持独立；新功能另发 R146。

## 已实现的功能

| 功能 | 本版行为 |
|---|---|
| TF / NAS | 分别读取设备当前曲库，分别缓存；APP 可切换来源或跟随设备 |
| 总列表 / 一级 / 二级 | 请求设备切换范围，一级选择目录、二级选择父目录及子目录 |
| 歌曲 / 歌手 / 专辑 / 年代 | 使用设备分类顺序和成员顺序，不在手机重新推导年代分组 |
| 搜索 | 综合、歌名、歌手、专辑、年份及年代；按文字包含匹配，不含拼音转写 |
| 当前列表 | 显示设备当前浏览模式、分组、目录或设备搜索结果；打开时同步，此后每秒检查 |
| 搜索点歌 | 手机仅筛选当前列表；发送原始设备行位置和曲目 ID，播放范围仍由设备执行 |
| 过期操作 | 设备校验来源、索引代次、列表状态标识及曲目 ID；变化后重新同步 |
| 本地缓存 | 完整曲目、分类及成员数据；CRC32 校验分页传输，SHA-256 校验落盘数据 |
| 日常重开 | 先校验缓存身份，命中则不下载完整索引，只更新当前列表 |
| 索引更新 | 内容标识变化后完整下载、校验，再原子替换；本版没有差量下载或断点续传 |
| 离线 | 可浏览缓存全库及分类、搜索；禁止选歌和修改设备列表 |
| NAS 格式 | 沿用设备限制，只开放 NAS MP3 / FLAC 点歌 |
| BLE | 原控制功能保留；曲库同步、列表切换与选歌仅通过 Wi-Fi |

“总／一级／二级”是目录播放范围；“歌曲／歌手／专辑／年代”是浏览分类。
按设备现有规则，一级／二级范围下使用目录歌曲列表，不开放跨目录分类切换。
在歌手或专辑页面搜索会筛选符合条件的分组，进入后继续搜索对应歌曲。
设备搜索模式下，APP 进一步筛选设备当前搜索结果，不修改设备的搜索键盘条件。

## 文件选择

- `FakePod_R146_APP_Music_Library.patch`：仅用于固件 R145.23.1 目录。
- `Android_V0534_Music_Library_AppOnly.patch`：仅用于 APP V0.5.33 目录，不依赖 `tests` 目录。
- 两个 `Source.zip`：对应完整源码，ZIP 根目录即项目根目录。
- `validation/`：主机验证源码与结果，独立于项目补丁；不需要复制到本机项目。

使用补丁或完整源码二选一，不要把完整源码覆盖后再重复应用同一个补丁。
如果两端本地还有未合入的修改，请先保留它们；检查失败时不要强制应用。

## 1. 应用固件补丁

```powershell
cd D:\ESP32\FakePod_Custom\FakePod_Custom
$patch = "$env:USERPROFILE\Downloads\FakePod_R146_APP_Music_Library.patch"
git -c core.whitespace=cr-at-eol apply --ignore-space-change --check $patch
if ($LASTEXITCODE -eq 0) {
    git -c core.whitespace=cr-at-eol apply --ignore-space-change $patch
}
```

按现有 PlatformIO / ESP-IDF 流程编译并烧录。本项目 PlatformIO 环境是 `fakepod_nano`，
若命令行已有 `pio` 可先运行 `pio run -e fakepod_nano`。
建议先更新固件，再安装 APP。

## 2. 应用 APP 补丁

```powershell
cd D:\ESP32\FakePod_Remote_V0_1_Android_Source
$patch = "$env:USERPROFILE\Downloads\Android_V0534_Music_Library_AppOnly.patch"
git -c core.whitespace=cr-at-eol apply --ignore-space-change --check $patch
if ($LASTEXITCODE -eq 0) {
    git -c core.whitespace=cr-at-eol apply --ignore-space-change $patch
}
```

`--ignore-space-change` 用于兼容本机 LF / CRLF 换行差异。
检查成功并应用后，再编译安装；检查失败时不要继续安装旧源码。

```powershell
$env:JAVA_HOME = "C:\Users\YCB\.jdks\ms-17.0.20.1"
$env:Path = "$env:JAVA_HOME\bin;$env:Path"
.\gradlew.bat :app:installDebug
```

应用版本名为 `0.5.34-music-library`，versionCode 为 48。

## 3. 手机使用

1. 连接 FakePod Wi-Fi 遥控，设备处于音乐应用。
2. 打开主界面的“音乐曲库 · 浏览与选歌”。打开曲库会同时打开设备曲库界面。
3. 首次同步会显示阶段与进度，完成前保留旧显示但不允许点歌。
4. 使用“来源”选择跟随设备、TF 或 NAS。切换来源仅改变浏览源，不自动切歌。
5. 选择总／一级／二级范围；一级选择目录，二级先进入父目录，再选二级目录。
6. 总列表可切换歌曲、歌手、专辑、年代。搜索仅筛选当前页面。
7. 点歌后设备执行原有选歌与播放逻辑，下一首、循环和随机仍由设备管理。
8. 索引变化或通信失败时点“同步”。离线状态只浏览缓存，连接后重新校验才允许操作。

第一次同步的实际耗时、RAM 变化和同时播放的稳定性需要实机测量，未给出性能保证。
本版通过设备内存中的 V2 Catalog 导出浏览数据，不重新扫描 NAS，也不修改原 V2 索引文件格式。

## 验证情况与边界

已通过：

- Android 全部 Kotlin 源码在 Kotlin 1.9.23、Android API 35 及现有依赖下的编译类型检查。
- 实际新增固件接口在原生 C++ 主机桩环境中的编译与运行。
- 固件真实分页导出 → Android 解析器与缓存客户端的联动测试。
- 长中文标签跨页、分类成员顺序、四维搜索、CRC 错误、缺片、截断数据拒绝。
- 过期列表及曲目不匹配拒绝、目录范围切换、待处理请求断开取消。
- TF / NAS 缓存分离、缓存复用、离线重新打开及本地完整性校验。
- 两份补丁在独立基线副本应用，应用后源码与目标逐字节比较；完整源码 ZIP 校验。
- 音频源、解码器、音频服务、SEEK 协议及 APP SEEK 控制器与稳定基线逐字节一致。

未进行：完整 Gradle APK 打包、完整 ESP-IDF 交叉编译、手机 UI 实机测试及板端播放测试。
主机环境对 LVGL、任务、存储、Android Handler/文件重命名等使用桩；不能代替真实调度和硬件验证。

## 本地验收

1. 两端正常编译，APP 版本是 0.5.34。
2. TF 首次同步歌曲数量、标题和分类与设备一致；关闭重开无需完整下载。
3. NAS 同样验证；测试重名歌手、同名专辑、多歌手及未知年代。
4. 测试总列表、一级递归、二级直属目录；点歌后测试下一首、上一首、随机和自然播完。
5. 歌手／专辑／年代内搜索并点歌，确认播放范围不意外回到总列表。
6. 设备切换分类或目录，APP 自动更新；APP 请求执行期间设备切换列表，旧请求应拒绝。
7. 下载中断网、重新连接、索引更新后重同步；原缓存不被半份下载覆盖。
8. NAS 播放时同步曲库，观察声音、遥控连接、RAM_MON 与缓冲最低值。
9. 回归本地 SEEK、NAS MP3 SEEK、两类 NAS FLAC SEEK、暂停、视频和 NSF。

这轮不修改 NAS SEEK 算法、HTTP Range、音频缓冲配置或解码器。
