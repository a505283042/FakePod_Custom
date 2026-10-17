# FakePod R1464 / Android V0.5.36：曲库检查与高亮修复

基线：上一轮保存的 R1463 Android 与设备端源码包。本轮是该基线之后的增量修改。

## 本轮已修改

| 位置 | 检查结果 | 修改 |
| --- | --- | --- |
| APP 顶部分类 | 原来只在 mode=4 的分组歌曲页高亮，歌手/专辑/年代分组列表这层没有覆盖 | 分组列表和分组内歌曲页均按当前播放归属高亮分类控件 |
| APP 分组行 | 旧代码已有成员判断，但重绘条件只比较曲目编号、列表内容和可操作状态，未完整覆盖分组索引替换等变化 | 统一歌曲行、分组行、父级控件和定位使用的高亮快照；索引、来源、分类和播放归属变化均触发重绘 |
| APP 可见样式 | 原背景透明度较低 | 改为较明显的蓝色背景，保留蓝色粗体和 ▶ 标记 |
| APP 切歌确认 | 曲库状态主要依赖每秒轮询 | 收到曲目/来源/上下文变化后，合并发起状态确认；忙时等待，不中断曲库下载；仍以设备返回状态为准 |
| APP 定位 | 定位失败也会被记成已完成；旧滚动任务可能落后于连续切歌 | 找到目标后才记录定位；以任务序号、列表、索引、视图、搜索词和播放快照检查过期任务 |
| APP 查询 | 每个列表项重复归一化搜索词，并为分组构造临时曲目列表 | 每次搜索只归一化一次；逐条短路匹配；空搜索直接复用列表 |
| APP 分组归属 | 画行、父级和定位分别查找成员 | 每个索引实例缓存当前曲目所属分组，供各层复用；支持同一首歌属于多个歌手分组 |
| 设备切歌调度 | 原函数可能从播放/遥控任务调用 lv_async_call | 外部任务只置原子标记；现有 20ms LVGL 定时器合并处理最新状态，减少重复排队 |
| 设备分组定位 | 重复定位同一首歌时重新扫描分类成员 | 缓存首个匹配分组的 ID，按浏览来源、generation、曲目、分类失效；不缓存 Catalog 裸指针；其他合唱歌手行仍正常高亮 |
| NAS 文件夹数量 | 为获得目录总数，逐个调用从头扫描的 option_at，重复遍历大量曲目 | 在相同目录枚举逻辑中增加单遍计数路径，不构造路径字符串；原一级/二级、直接歌曲和深层目录规则保持 |
| 版本识别 | R1463 源码包中 app/build.gradle.kts 仍为 V0.5.34，固件版本入口仍为 R146.2 | 本轮明确改为 APP V0.5.36 / versionCode=49，固件 R146.4，便于确认实际安装和烧录版本 |

关于“分组行一直没有高亮”的真机根因：源码可确认顶部分类漏层及行重绘覆盖不足，但没有手机现场日志，不能仅凭代码声称已经复现并确认全部原因。请按后面的真机项目验证。

## 曲库已有机制与后续优化点

现有本地增量扫描按文件签名复用技术参数、标签和封面信息；MP3/FLAC/Opus 需要深度解析时共享一次文件打开。设备曲库 UI 已使用虚拟行；APP 已有内存视图缓存和本地持久化索引；NAS cooperative loader 会分批让出 SD 锁。本轮优先修复 UI 热路径，没有重做这些机制。

仍可继续优化：

1. NAS 的随机目录选项读取、二级父目录筛选和目录有效性检查，部分路径仍会重新扫描曲目。可以按 Catalog generation 构建紧凑的目录偏移/计数索引；需结合 PSRAM、音频并发和设备实测决定，不能从单遍计数的测试直接推算整页耗时。
2. APP 的内存视图缓存限制为 8 个，但 view-*.rows 等磁盘缓存没有统一总量淘汰策略。后续可增加数量/容量上限，同时保留本地与 NAS 的可用离线缓存。
3. NAS 首次加载包含磁盘读取、校验和分组构建。建议继续按阶段日志计时，再决定是否保存分组索引或增加预生成数据；当前没有新的设备耗时数据。

以上三项为检查结论和后续建议，本轮没有把它们混入代码改动。

## 已执行验证

- Kotlin：使用 Kotlin 1.9.23 编译器和 Android API 35 类库，编译 MusicLibraryModel.kt、MusicLibraryClient.kt、MusicLibraryDialog.kt 和回归用例；通过。工程 Gradle Kotlin 插件仍保持原来的 2.0.21。此项是核心文件的宿主编译，不是 APK 构建。
- Kotlin 回归：歌曲、歌手/专辑/年代行、父级分类控件、连续曲目变化、同编号索引替换、同名分组、多个歌手成员、不同来源、离线/无效状态、搜索和缓存复用；通过。
- C++ 宿主回归：从生产源码提取分组缓存/定位/切歌处理函数编译运行，覆盖合唱歌手、generation 和来源失效、搜索/目录页保护、10000 次通知合并处理；通过。
- NAS 目录差异测试：空库、中文目录、大小写、嵌套、直接歌曲以及 50 组随机目录，新计数与原逐项枚举结果一致；通过。
- 200 个目录、2000 首曲目的测试夹具：统计路径查询次数从 243598 降至 2399。这是宿主夹具的调用次数，不是播放器实测速度或整页提升倍数。
- 既有 NAS 服务器切换播放绑定回归；通过。
- 两份补丁正向检查、反向检查、正向应用后与交付源码逐文件比对；通过。
- 两份 ZIP 完整性检查；通过。

未完成：完整 Gradle APK 构建、ESP-IDF/PlatformIO 固件构建、手机/播放器真机验证。没有生成新的 APK 或可刷写固件。

## 应用 Android 补丁

仅适用于已经合入 R1463 的项目。先把本轮 .patch 下载到 Downloads；如果文件被浏览器改名为 (1)，相应调整路径。

```powershell
cd "D:\ESP32\FakePod_Remote_V0_1_Android_Source"
$patch = "$env:USERPROFILE\Downloads\FakePod_Android_V0536_R1464_Library_Highlight.patch"
git -c core.whitespace=cr-at-eol apply --ignore-space-change --check $patch
if ($LASTEXITCODE -ne 0) { throw "补丁检查未通过，请停止，不要强行应用" }
git -c core.whitespace=cr-at-eol apply --ignore-space-change $patch
if ($LASTEXITCODE -ne 0) { throw "补丁未成功应用" }
.\gradlew.bat assembleDebug --no-configuration-cache --no-parallel --max-workers=1
if ($LASTEXITCODE -ne 0) { throw "编译未成功，请不要安装旧 APK" }
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $adb devices
& $adb install -r ".\app\build\outputs\apk\debug\app-debug.apk"
```

若检查未通过，先核对是否已经应用：

```powershell
git -c core.whitespace=cr-at-eol apply --ignore-space-change --reverse --check $patch
```

返回 0 表示此补丁中的修改已经存在，不要重复应用。当前项目没有 Git 仓库也能使用 git apply，不要再用 git diff --check 判断补丁是否成功。

## 应用设备端补丁

```powershell
cd "D:\ESP32\FakePod_Custom\FakePod_Custom"
$patch = "$env:USERPROFILE\Downloads\FakePod_R1464_Library_Focus_Count.patch"
git -c core.whitespace=cr-at-eol apply --ignore-space-change --check $patch
if ($LASTEXITCODE -ne 0) { throw "补丁检查未通过，请停止" }
git -c core.whitespace=cr-at-eol apply --ignore-space-change $patch
if ($LASTEXITCODE -ne 0) { throw "补丁未成功应用" }
```

然后使用你原来的 PlatformIO/ESP-IDF 环境编译、烧录。源码包没有包含新的可刷写 .bin，请从本轮源码重新编译。

## 真机验证

1. 安装后在 Android 应用信息中确认版本为 0.5.36-library-highlight；设备启动版本为 R146.4。
2. 总列表中播放歌曲，打开歌手、专辑、年代：所属分组行应出现蓝色背景、蓝色粗体和 ▶；当前分类控件也应高亮。
3. 进入所属分组后，歌曲行与上方父级分类控件应同时高亮；进入不包含当前歌曲的另一组，不能错误高亮该父级。
4. 使用一首合唱歌曲检查多个相关歌手分组；同名专辑/歌手应按组 ID 和成员匹配，不按名称猜测。
5. 播放 A 再切 B、连续切几首，确认高亮和定位跟随最后一首；手动滚动或输入搜索后不能被过期任务拉回。
6. 本地、NAS 分别验证。浏览 NAS 而播放本地同编号歌曲时，NAS 不得错误高亮。
7. NAS 一级/二级目录数量、目录内直接歌曲数量应与原来一致；二级页面不把更深层歌曲误算为直接歌曲。

源码包排除了旧构建产物、缓存和日志；各源码包根目录均为可用工程目录。Windows 上已有 local.properties、签名、SDK 与本地配置请保留，优先使用增量补丁。
