# R46.0.143 + Android V0.5.32：APP控制本地音乐SEEK

手机遥控页进度条可以拖动。拖动预览时间，松手只发送一次，显示“定位中”，收到设备成功定位及后续进度后恢复同步。暂停时定位保持暂停。断线、切歌、切换音源会取消旧请求；10秒未确认恢复设备进度并提示。

只对设备报告支持定位的本地音乐启用。部分没有有效定位表的FLAC不可定位。NAS、视频、NSF未开放。Wi-Fi和BLE均支持。旧固件连接新APP时仍显示普通进度，不发送新命令。

## 应用命令

补丁基线：固件R142，APP V0531。两个补丁下载到Downloads后，在PowerShell分别执行：

```powershell
cd D:\ESP32\FakePod_Custom\FakePod_Custom
git apply --check "$env:USERPROFILE\Downloads\R46143_APP_Local_Music_Seek.patch"
git apply "$env:USERPROFILE\Downloads\R46143_APP_Local_Music_Seek.patch"
```

按原PlatformIO环境编译、烧录固件。再更新APP：

```powershell
cd D:\ESP32\FakePod_Remote_V0_1_Android_Source
git apply --check "$env:USERPROFILE\Downloads\Android_V0532_Local_Music_Seek.patch"
git apply "$env:USERPROFILE\Downloads\Android_V0532_Local_Music_Seek.patch"
$env:JAVA_HOME = "C:\Users\YCB\.jdks\ms-17.0.20.1"
$env:Path = "$env:JAVA_HOME\bin;$env:Path"
.\gradlew.bat :app:installDebug
```

也可以分别使用两份完整源码；完整源码与补丁二选一。覆盖安装APP保留设置。

## 实现

复用player_transport_seek_ms与AudioTask现有定位路径，不增加解码器。命令0x18为17字节：命令、曲目uint32、播放会话uint32、预期seek_revision uint32、目标毫秒uint32，整数小端。固件在播放控制锁内核对本地来源、曲目、会话、定位版本、能力、播放/暂停状态及范围，避免旧请求影响新歌。

保留20字节基础状态长度，增加NAS/定位能力位；新增19字节0x05扩展状态，包含能力、曲目、会话、seek_revision、最后成功定位目标。BLE无需扩大MTU。APP待成功定位版本变化并收到更新基础进度才结束预览，避免旧进度回跳。

## 验证

固件实际函数提取主机测试通过，含ASan/UBSan（环境限制关闭LeakSanitizer）：命令长度、跨语言小端、NAS/曲目/会话/定位版本拒绝、暂停、忙状态、首尾边界、锁释放、扩展状态。APP控制器测试通过：松手仅一次、预览、确认顺序、超时、取消、切歌/音源/会话变化、暂停和边界。半帧接收、重连、Wi-Fi/BLE仲裁、视觉控制回归通过。全生产Kotlin源码以Android35类型编译通过，资源ID使用验证桩。

未执行完整ESP-IDF构建、Gradle APK打包和真机验证。建议更新两端后用本地MP3及可定位FLAC分别测试播放/暂停拖动、拖到首尾、拖动时切歌、断线重连；NAS进度条应不可拖动。
