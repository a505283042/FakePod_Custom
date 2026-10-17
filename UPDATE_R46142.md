# R46.0.142 + Android V0.5.31 · 手机磁带封面位置 / 三种频谱模式

## 使用

手机连接设备后，在遥控页“设备视图”选“磁带”，出现“封面位置：↑上移、↓下移”，每次复用设备原有4px步长及安全裁切边界。选择“频谱”，出现“频谱模式”：分段柱状、横向镜像、霓虹山脊。选择按设备实际回报同步，不把发送当成已应用。本地触屏切换样式后手机也更新。

锁屏/息屏时新操作禁用；设备音乐不在前台、切到其他视图时对应行隐藏。位置调整沿用设备当前封面的临时偏移，未新增逐曲持久保存。频谱沿用原有状态/取样/FFT，不增加音频处理。

## 安装

本次要同时更新固件与APP：固件基线R141、APP基线V0530。两个补丁放下载目录：

```powershell
cd D:\ESP32\FakePod_Custom\FakePod_Custom
git apply --check "$env:USERPROFILE\Downloads\R46142_Remote_Visual_Controls.patch"
git apply "$env:USERPROFILE\Downloads\R46142_Remote_Visual_Controls.patch"
```

固件按原PlatformIO环境编译烧录。然后更新APP：

```powershell
cd D:\ESP32\FakePod_Remote_V0_1_Android_Source
git apply --check "$env:USERPROFILE\Downloads\Android_V0531_Remote_Visual_Controls.patch"
git apply "$env:USERPROFILE\Downloads\Android_V0531_Remote_Visual_Controls.patch"
$env:JAVA_HOME = "C:\Users\YCB\.jdks\ms-17.0.20.1"
$env:Path = "$env:JAVA_HOME\bin;$env:Path"
.\gradlew.bat :app:installDebug
```

也可分别解压完整源码使用；覆盖源码与补丁二选一。APP覆盖安装保留设置，不卸载。

## 实现/兼容

Wi-Fi/BLE命令0x16：两字节[命令,0上/1下]；0x17：[命令,0/1/2频谱样式]。扩展状态0x04为四字节[type,version,capability_bits,style]，位0支持磁带位置、位1支持频谱。保留现有20字节播放状态和6字节0x03 UI状态，旧APP忽略新事件即可；新APP连接旧固件不显示新控制，原有控制仍可用。

网络/BLE任务只提交原子邮箱，LVGL线程消费后才修改图片/样式。磁带待执行净步数限制±8，同一轮相反方向可合并抵消；频谱为最新选择优先。超过500ms的陈旧视觉请求丢弃，锁屏/非音乐场景不执行排队视觉操作。共享设备触屏的移动/样式函数，复用安全边界、截图缓存失效和布局重建。没有新的常驻图像缓冲或频谱任务。

## 验证与边界

固件主机测试提取实际移动、样式、原子邮箱和BLE/Wi-Fi命令分支：4px方向、边界、邮箱上限/过期/锁屏拒绝、三种模式、重复模式不重建、命令长度/参数0..255一致性通过。APP提取实际发送和扩展包解析：能力协商、旧固件不发新命令、视图/电源/锁屏/音乐场景限制、BLE/Wi-Fi字节发送、异常扩展包拒绝通过。半帧和自动重连、Wi-Fi/BLE状态仲裁回归通过。Android35全Kotlin类型编译通过（资源ID验证桩）；补丁应用后文件一致性、ZIP校验通过。

没有完整ESP-IDF固件编译、Gradle APK打包或真机UI测试，不能将主机测试视为真机已稳定。既有Wi-Fi首次关联失败、真实TCP断开和FLAC首PCM超时仍须按日志验证。

刷入R142、安装V0531后，先测试磁带上/下与安全边界，再测试三种频谱、设备触屏切换后的手机同步，以及Wi-Fi/BLE分别连接。退出到设置/视频/NSF或锁屏时不应能误操作。
