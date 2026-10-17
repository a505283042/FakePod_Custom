# FakePod R147.0 / Android 0.5.42：发布前队列与恢复修复

补丁基线：固件 R146.8、Android 0.5.40；本轮需两端一起更新。

## 修复内容

1. APP 本地独立队列的上一首／下一首与自然续播

原来顺序切歌仍走设备原有本地目录，APP 捕获的队列位置也没有同步更新，可能错播到设备目录，或者随后 EOF 被误判为过期。
现在存在 APP 捕获队列时，统一通过捕获队列移动并更新位置；顺序与列表循环的 EOF 进入同一队列路径。随机播放原有队列入口保持使用捕获队列。捕获队列代次失效时停止本次切歌，不退回设备目录。没有 APP 队列时保留原生设备切歌行为；设备点歌仍可接管队列。

2. BLE 会话清理后恢复 Wi-Fi 搜索

把恢复搜索放入统一的 GATT 会话重置入口，覆盖手机蓝牙关闭、GATT 出错及会话关闭路径。只有遥控仍处于活动期且 Wi-Fi 尚未接管时才恢复搜索／启动工作线程；已连接 Wi-Fi 时不另起线程，后台超过既有宽限期后不因清理会话而恢复连接。BLE 正常就绪后仍暂停 Wi-Fi 搜索。

3. APP 从本地转播 NAS 前保存本地位置

在实际选择 NAS 队列之前、播放器串行锁内部，补上原设备 NAS 点歌路径已有的本地曲目 checkpoint。NAS → NAS、本地点歌、非法参数或未取得锁不会触发该保存。保存失败与原设备路径保持一致：记录日志，不阻止 NAS 选歌。

设备视图保留 R146.8 行为：熄屏／AOD 时可记住最后请求，正常显示且回到可切换的音乐页面时执行；不主动唤醒、解锁。本轮没有应用已撤回的亮屏限制。

## 应用方法

先解压交付包，把脚本与两个补丁放在同一文件夹，终端进入该文件夹运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\Apply_FakePod_R1470.ps1" -Target Android
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\Apply_FakePod_R1470.ps1" -Target Firmware
```

脚本默认 Android 工程位置 `D:\ESP32\FakePod_Remote_V0_1_Android_Source`，固件位置 `D:\ESP32\FakePod_Custom\FakePod_Custom`。
补丁检查不匹配会停止，不要强行应用或安装旧 APK。完整源码也已包含在包内；使用完整源码时保留本机 SDK／签名和现有 PlatformIO 配置。

Android 看到 BUILD SUCCESSFUL 后，连接手机并开启 USB 调试：

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $adb devices
# 确认手机显示 device 后再运行：
& $adb install -r "D:\ESP32\FakePod_Remote_V0_1_Android_Source\app\build\outputs\apk\debug\app-debug.apk"
```

固件补丁成功后，在原 VS Code 工程中用原 PlatformIO 环境编译并上传。预期版本：Android 0.5.42，固件 R147.0。

## 已验证

- 真实 player_state 移动函数与 player_transport 的手动切歌、自然 EOF 入口主机测试通过：Local／NAS 捕获队列、顺序末尾停止、列表循环回首、单曲重播、随机入口与队列衔接、过期 EOF 忽略、失效代次拒绝、无 APP 队列时原生回退。底层音频输出及随机候选算法使用测试替身，并非真机音频或随机分布测试。
- 实际 APP NAS 选择包装函数测试通过：只在 Local → NAS 时、锁内保存；NAS → NAS／Local 不重复保存，锁失败／非法参数不保存，保存失败不遗留锁。
- 提取实际 BLE 清理和手机蓝牙关闭方法，与真实 TCP 传输及本机模拟设备联测通过：恢复暂停搜索、GATT 重置恢复、现有 Wi-Fi 保持、Wi-Fi 接管／后台暂停不额外重连。Android GATT／网络环境为测试替身，未覆盖真实手机系统行为。
- 将旧逻辑分别放回后，三项新增回归均按预期失败；修复后通过，确认测试能检出本轮问题。
- 涉及 Android 客户端、传输、UI 的 Kotlin 编译检查通过（UI 存在占位类，非完整 Gradle APK）。独立浏览、来源选择、高亮回归通过。
- NAS 服务切换绑定、BLE 节能诊断既有回归通过。
- 两端补丁前向检查／应用／反向检查和目标文件一致性检查通过；源码与交付 ZIP 完整性检查通过。
- 尚未完成完整 Gradle APK／PlatformIO 固件构建，未进行真实设备 BLE/Wi-Fi 切换、NVS 断电恢复或耗电实测。此包仍需编译与真机验收后再标正式稳定版。

## 真机重点验收

1. 设备停在本地 Z 目录，APP 在 A 目录点歌；手动上一首／下一首、自然续播都留在 A 队列，歌曲高亮随实际播放更新。
2. 分别测试顺序、列表循环、单曲、随机；设备再主动点歌后，后续切歌按设备新队列。
3. APP 通过 BLE 连接时关闭手机蓝牙，设备开启 Wi-Fi；APP 应能自动恢复 Wi-Fi 遥控。APP 后台超过 30 秒后保持暂停，回前台再恢复。
4. 本地播到中间曲目，由 APP 转播 NAS，正常重启并在不开 Wi-Fi 的情况下验证本地最后曲目；异常断电仍受 NVS 写入成功与否影响。
5. NAS 来源反复重开仍保持，分类各自浏览；熄屏选择视图不唤醒，恢复后切到最后选择。
