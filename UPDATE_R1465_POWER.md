# FakePod 通信与后台耗电审计：APP V0.5.37 / 固件 R146.5

基线：已确认可用的 Android V0.5.36（包含 Current_Compatible 修复）与设备 R146.4。补丁用于这些基线；不能用编译成功代替补丁成功。完整源码包包含此前的曲库高亮与目录优化。

## 本轮已经落实

| 路径 | 原行为 | 新行为及影响 |
|---|---|---|
| APP 后台遥控 | Application 持有 Wi-Fi/GATT，Activity 隐藏后仍保持连接和重连 | Activity 不可见超过 30 秒后，停止遥控 Wi-Fi 工作线程、BLE 扫描/GATT、重连与曲库会话；返回前台恢复。30 秒内切换窗口沿用会话。手机息屏也适用；设备自身继续播放。后台 NAS 扫描有独立 Service、HTTP/SMB/SSH 和锁，不依赖遥控会话，保留其任务。 |
| BLE 已连接时的 Wi-Fi 寻找 | 设备无线互斥，但手机仍不断尝试 TCP/UDP | 已完成 BLE 初始状态读取后暂停 Wi-Fi 寻找，工作线程条件等待，无周期唤醒；BLE 断开时恢复寻找，健康的已有 TCP 不受暂停寻找影响。 |
| 不在线设备的寻找 | 约 0.5–0.7 秒后继续连接/广播，一个发现窗口中多次发相同广播 | 失败后等待 0.7/1.4/2.8/5.6/11.2/15 秒，封顶 15 秒；网络对象变化、手动恢复或传输模式恢复后重置；每轮只发送一轮去重目标广播。找不到设备很久后，重新出现的发现时间可能增加。 |
| 停止/重启并发 | 单个 running 布尔值可能被新线程复用；旧线程退出时会影响新会话 | 增加会话世代、socket 归属检查与停止时队列清空；旧线程不能清理新连接，停止后的控制命令不会带到新会话。 |
| BLE 连接参数 | 未按空闲/传输区分请求优先级 | 空闲请求 Android LOW_POWER；曲库分页期间请求 BALANCED，结束/失败恢复 LOW_POWER；相同请求去重。参数最终由手机/外设协商，不等于控制器休眠已开启。 |
| APP 曲库状态查询 | 已就绪时每秒一次 | 稳定时 5 秒兜底；正在加载仍每秒；切歌/来源/上下文变化合并 80ms 后主动查询，不等待 5 秒。设备端单独改变浏览层级可能最多约 5 秒后同步。 |
| BLE 状态计算 | 每个约 10ms 系统循环重复构造 SEEK、控件状态并查询加密连接 | 稳定时 100ms 采样；没有元数据待发送时也跳过重复加密描述查询。首连/强制刷新不受节流限制；元数据有待发分片时保持原快速发送路径。 |
| BLE 曲库空闲泵 | 无请求也检查加密状态、尝试取帧 | 无请求、无部分帧、无待处理回复时直接返回；请求进入后仍按原分片/背压规则传输。 |
| BLE SEEK 扩展 | 内容不变仍每 2 秒补发 | 改为每 15 秒校验补发，内容变化立即进入下一次采样发送。原主状态、控件与元数据可靠性重放保持。 |
| Wi-Fi 遥控任务 | 无手机时每 100ms 唤醒；每轮重置曲库遥控 | 无手机最长阻塞 500ms，只有进入空闲时重置一次；有手机 250ms，曲库回复待发时 20ms。select 的 socket 事件可提前唤醒，非固定等待 500ms 才接收命令。 |
| Wi-Fi SEEK 扩展 | 每次主状态发送都重发同样的 SEEK 扩展 | 首连或修订/支持能力变化时才发送。TCP 保序可靠，主状态 1.5 秒保活保持，继续兼容旧 APP 的 3 秒失联阈值。 |
| Wi-Fi UI 状态 | 控件/频谱状态可能等下一次主状态保活才发送 | 控件/频谱变化也进入发送条件，减少因节流导致反馈迟缓。 |
| APP 元数据解析 | BLE 可靠性重放的重复分片会触发重复合并/UTF-8 解码/界面刷新 | 相同序列、同一位置且字节相同的分片直接忽略；不同内容和新序列仍正常处理。 |
| APP 隐藏设置页 | 每次状态包都读取当前 Wi-Fi SSID、权限/定位服务，即使未打开设置 | 设置页关闭时直接返回；连接状态颜色没变化时复用背景，减少 Drawable 分配。 |

上述间隔对比是源码调度次数，不是实测电池续航：稳定曲库查询约 60→12 次/分钟；BLE 辅助快照约 100→10 次/秒；无手机的 Wi-Fi select 超时唤醒上限约 10→2 次/秒。实际还受事件、调度、传输和手机系统影响。

## 全盘审计的其他结论

1. **Wi-Fi Modem Sleep 被显式关闭**：`src/system/wifi_service.cpp` 调用 `esp_wifi_set_ps(WIFI_PS_NONE)`，注释表明这是此前 AP/断连 A/B 诊断留下的配置。此项很可能是设备联网待机功耗的重要来源。本轮主补丁保留这个已使用的网络策略，另附独立 `FakePod_R1465_Optional_WiFi_MinModem.patch` 供单独 A/B 测试；该试验改成 MIN_MODEM，作用于所有 Wi-Fi 流量，包含 NAS，并非已验证的自适应策略。主源码 ZIP 不含该可选补丁效果。
2. **BLE 控制器 Modem Sleep 未开启**：实际 USB CDC 配置中 `CONFIG_BT_CTRL_MODEM_SLEEP` 关闭。仅延长连接参数和减少 notify 不会自动开启控制器休眠。需要先核对当前主板低频时钟、ESP-IDF/NimBLE 配置及唤醒稳定性后再调整；本轮不提交未经验证的 sdkconfig 组合。
3. **CPU 运行在 240MHz，PM 未开启**：`CONFIG_PM_ENABLE` 关闭。减计算并不保证 CPU 自动降频。音频、视频、显示和 SPI/SD/I2S 需要按任务持有频率/APB 锁后才能安全启用 DFS/自动休眠；直接全局降频可能导致实时任务欠载，需另做性能与电流 A/B。
4. **FFT 已有可见性门控**：spectrum_view 隐藏/暂停路径会关闭音频频谱旁路；没有把正常播放所需解码/I2S误当冗余计算删除。
5. **IMU 软件采样已有门控**：只有允许手势、Music/Ebook、亮屏解锁且无菜单时才进入 100Hz 主机读取；锁屏/息屏停止该段采样。传感器本身是否还在高 ODR/陀螺仪模式需结合驱动和硬件电流进一步验证，不把“没读 I2C”等同于传感器断电。
6. **电池采样与持久化已有降频**：电池约 2 秒采样；运行态持久化观察约 250ms且 dirty 管理，RAM 监控约 1 秒采样。均不是简单从 10ms 无脑重复写 NVS。
7. **曲库匹配已有缓存**：当前歌曲所属歌手/专辑/年代成员关系、筛选基础列表已缓存；前一轮优化继续保留，没有为省电改回文字匹配或漏掉合唱成员。
8. **NAS 长任务锁用途明确**：手机 NAS 扫描期间的 PARTIAL_WAKE_LOCK 与 HIGH_PERF Wi-Fi lock用于显式用户扫描，完成/异常由 Service释放。它们不是常驻遥控锁；本轮保留，以免后台扫描半途断流。仍需真机验证扫描结束和系统取消后的锁释放。
9. **无线互斥继续保留**：设备已有先停 BLE 再启 Wi-Fi 的接管流程；不会为了减少连接而周期性开关设备 Wi-Fi，也不会在 NAS 播放途中自动关网。保持健康长连接通常优于反复连接/认证/发现服务。

官方机制参考：https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html 。MIN_MODEM 按 AP DTIM 接收，可能增加接收等待；节电多少依赖 AP 配置和实际流量。

## 验证范围

- Android 核心通信、曲库、MainActivity、Application 使用 Android API 35 和 Kotlin 1.9.23 宿主编译；NAS 设置对话框及资源 R 使用占位类型，仅用于检查本轮接入点。不是完整 Gradle APK 构建。
- 实际 FakePodWifiTransport 使用本地 TCP server与最小 Android 网络/时钟替身运行：暂停发现、已有连接保留、停止/多次快速重启、停止后无后台重连；通过。不是 Android 网络栈实机测试。
- 实际 WifiFrameReader：多次超时之间保持半帧、后续帧与 EOF；通过。退避上下界；通过。
- 从生产固件提取 BLE 状态调度函数，经 g++ 宿主执行：稳定节流、曲目修订、15 秒刷新、快速元数据、取消订阅和 tick 回绕；通过。不是 ESP-IDF 整体固件编译。
- 前一轮曲库高亮 Kotlin、设备高亮/定位、NAS 目录计数和 NAS 播放绑定回归；通过。
- 补丁正向检查、应用后逐文件比对、反向检查与 ZIP 完整性检查；通过。
- 未完成：完整 APK/固件构建、手机/播放器真机、电流及续航测量。没有声称实际节电百分比，也没有生成 APK 或可烧录二进制。

## 应用与安装

下载 Android 补丁、固件补丁和 `Apply_FakePod_R1465.ps1` 至同一目录。脚本每次只处理指定一端，失败不会继续编译。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$env:USERPROFILE\Downloads\Apply_FakePod_R1465.ps1" -Target Android
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$env:USERPROFILE\Downloads\Apply_FakePod_R1465.ps1" -Target Firmware
```

Android 成功后连接手机，确认 devices 有 `device`，再安装：

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $adb devices
& $adb install -r "D:\ESP32\FakePod_Remote_V0_1_Android_Source\app\build\outputs\apk\debug\app-debug.apk"
```

Firmware 脚本只应用与核对补丁；然后按原来 PlatformIO 工程配置编译烧录。若本地仍不是 R146.4，请使用完整 R146.5 源码包另目录构建并保留本地配置，不要强行套用补丁。

## 真机验证与电流比较

相同音量/曲目/亮度/AP，分别比较：无线全关的本地播放、BLE 已连接本地播放、Wi-Fi 已连接本地播放、NAS MP3、NAS FLAC、暂停息屏。每组固定等待稳定后连续记录电流；以外部电流表为准，电池百分比短期波动不能当测量结果。

重点验证：APP 后台 20 秒返回保持连接；后台超过 30 秒连接释放且设备不断播；再打开恢复；BLE 下首次曲库下载/缓存/切歌高亮；Wi-Fi 下快速切歌/SEEK；设备离线几分钟后回来；配网 BLE→Wi-Fi；NAS 扫描后台继续并能取消。

可选 Wi-Fi 省电补丁应单独应用与反向回退，测 NAS 缓冲/欠载、遥控延迟和曾出现的约 10 秒断连。它只改一处功耗模式与日志，不属于主补丁的必装项。
