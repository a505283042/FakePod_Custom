# 蓝牙栈迁移第一阶段：NimBLE → Bluedroid 双模

## Context

FakePod S31 目前的蓝牙主机栈是 NimBLE 单栈，承担三件事：手机 BLE 遥控 GATT 服务、BLE Wi-Fi 配网、手柄 HID Host（esp_hidh 的 NimBLE 后端）。第二阶段要做蓝牙音频发射（A2DP Source），而 NimBLE 不支持 Classic，因此必须先把主机栈迁到 Bluedroid 双模。

本阶段只做「迁移 + 回归」，不新增功能：

- 主机栈 = Bluedroid 双模；控制器 = BTDM；Classic 打开但暂不注册任何 Classic profile。
- 手机 BLE 遥控协议字节级不变：128 位 UUID、20 字节 Status 包、metadata 分片、曲库 0x31、Wi-Fi 配网 0x20、seek 0x18 全部保持。
- 手柄 HID Host 与 Controller 模式本阶段整体编译隔离（保留代码与 TODO），设置里不再提供「手柄」模式选项。
- A2DP/AVRCP 留到第二阶段，本阶段 `BT_A2DP_ENABLE=n`。

基线：主固件 R149.3 启动正常，PSRAM_free≈16057KB，内部 RAM 空闲约 156KB、播放最低约 60KB——Bluedroid 只能用内部 RAM，这是本阶段最大风险点。

## 改动 1：sdkconfig（配置切换）

文件：[sdkconfig.defaults](file:///D:/ESP32/FakePod_S31/sdkconfig.defaults)、[sdkconfig.ble.defaults](file:///D:/ESP32/FakePod_S31/sdkconfig.ble.defaults)

1. `sdkconfig.defaults` 第 71-85 行：整块删除 `CONFIG_BT_NIMBLE_*`，保留 `CONFIG_BT_ENABLED=y`、`CONFIG_BT_CONTROLLER_ENABLED=y`。
2. `sdkconfig.ble.defaults`：删除全部 NimBLE 项，以及 S31 porting_btdm 控制器里不存在的旧项（`CONFIG_BT_CTRL_BLE_ADV/SCAN/MASTER/DTM_ENABLE`、`BT_NIMBLE_TRANSPORT_*`、`BT_NIMBLE_MSYS_*` 等）；保留文件末尾 FREERTOS/LVGL/mbedTLS/TINYUSB 行。
3. 新增 Bluedroid 配置：

```
CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BTDM_CTRL_MODE_BTDM=y
CONFIG_BT_BLE_ENABLED=y
CONFIG_BT_GATTS_ENABLE=y
CONFIG_BT_GATTC_ENABLE=n
CONFIG_BT_BLE_SMP_ENABLE=y
CONFIG_BT_SMP_MAX_BONDS=4
CONFIG_BT_ACL_CONNECTIONS=1
CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y
CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n
CONFIG_BT_A2DP_ENABLE=n
CONFIG_BT_GATTS_SEND_SERVICE_CHANGE_AUTO=y
```

关键点（已核对 IDF 6.1 头文件）：`esp_ble_gap_config_adv_data_raw`、`esp_ble_gap_start_advertising` 被 `BLE_42_FEATURE_SUPPORT` 守卫，所以必须 4.2=y、5.0=n（两者互斥）；否则只能改用 `esp_ble_gap_ext_adv_*`，改造面更大。

4. 删除生成的 `sdkconfig.fakepod_s31`（根目录），强制按 defaults 重新生成；编译后核对生成文件里 `BT_BLUEDROID_ENABLED=y`、NimBLE 项不存在。

## 改动 2：ble_remote_service.cpp 分层与宏隔离

文件：[ble_remote_service.cpp](file:///D:/ESP32/FakePod_S31/src/system/ble_remote_service.cpp)、[ble_remote_service.h](file:///D:/ESP32/FakePod_S31/src/system/ble_remote_service.h)

- L27-49 宏块改写：
  - `FAKEPOD_BLE_FOUNDATION_ENABLED` = Bluedroid + BLE + GATTS + BLE 4.2 同时满足；
  - 新头文件：`esp_bt.h`、`esp_bt_main.h`、`esp_gap_ble_api.h`、`esp_gatts_api.h`；
  - `esp_hidh.h` 移入 HID 宏内，`nimble/*` 头全部移除；
  - 新增 `FAKEPOD_BLE_HID_HOST_ENABLED`（第一阶段固定 0）。
- 协议层不动：`build_status_packet()`（L390-462）、metadata/曲库/Wi-Fi 配网组包、命令码解析（L844-925）保持原样。
- 传输层重写（见改动 3），包括 `status_event_access`、`notify_phone_event`、`init_control_gatt_defs`/`register_control_service`、GAP 事件、广播、生命周期。
- HID/Controller 段用 `#if FAKEPOD_BLE_HID_HOST_ENABLED` 包住：L1146-1739（候选解析/扫描/连接/esp_hidh 回调）、L1721-1743、host 回调里的 HID 分支、stop_stack 的 HID 关闭、start_stack 的 `init_controller_hid_host`、`update()` 的 Controller 分支；避免留下调用 NimBLE API 的死代码。
- `set_mode(Controller)` 在 HID 不可用时：告警「手柄模式需 Bluedroid HID Host，第二阶段开放」，`g_mode` 保持/回退到 Off，`g_desired_enabled=false`，不触碰任何蓝牙 API。
- `state_name(Unsupported)` 文案改为「未启用蓝牙」。

## 改动 3：NimBLE → Bluedroid 映射

| 原 NimBLE | Bluedroid 替代 |
|---|---|
| `nimble_port_init` + Freertos 任务 | `esp_bt_controller_init` + `esp_bt_controller_enable(ESP_BT_MODE_BTDM)` → `esp_bluedroid_init` + `esp_bluedroid_enable`；事件在 BTC 任务回调，无需自建 host task |
| `ble_hs_cfg.sm_io_cap/bonding/mitm/sc/key_dist` | `esp_ble_gap_set_security_param`：IOCAP=NO_IO、AUTH_MODE=SC_BOND、INIT_KEYS=ENC\|ID、KEY_SIZE=16 |
| `ble_svc_gap_device_name/appearance` | `esp_ble_gap_set_device_name`；`esp_ble_gap_config_local_icon(0x0280)`（IDF 6.1 该接口收 uint16） |
| `ble_gatts_count_cfg/add_svcs` | `esp_ble_gatts_register_callback` → `esp_ble_gatts_app_register(0)` → REG_EVT → `esp_ble_gatts_create_attr_tab` → CREAT_ATTR_TAB_EVT → `esp_ble_gatts_start_service(handles[0])` |
| 属性句柄 | 6 条属性：0=服务声明、1=Command 声明、2=Command 值、3=Event 声明、4=Event 值、5=CCCD(0x2902) |
| `ble_gatts_access_cb` | READ_EVT → `esp_ble_gatts_send_response`（offset≠0 回 `ESP_GATT_INVALID_OFFSET`，否则回 20B）；WRITE_EVT 走现有命令解析，need_rsp 时回 OK/错误码 |
| `ble_gatts_notify_custom` + os_mbuf | `esp_ble_gatts_send_indicate(gatts_if, conn_id, handles[4], len, data, false)`，len≤20 |
| SUBSCRIBE 事件 | `ESP_GATTS_WRITE_EVT` 且 `handle==handles[5]` 时按 `value[0]&1` 更新订阅状态 |
| `ble_gap_conn_find(sec_state.encrypted)` | 由 `ESP_GAP_BLE_AUTH_CMPL_EVT` 成功置位、断连清零；权限层已由 `ESP_GATT_PERM_READ_ENC/WRITE_ENC` 强制 |
| REPEAT_PAIRING 删旧 bond | `ESP_GAP_BLE_AUTH_CMPL_EVT` 失败时一次性 `esp_ble_remove_bond_device` + `esp_ble_set_encryption(bda, ESP_BLE_SEC_ENCRYPT_NO_MITM)`；`ESP_GAP_BLE_SEC_REQ_EVT` → `esp_ble_gap_security_rsp(bda, true)` |
| `ble_gap_adv_set_fields/adv_start` | `esp_ble_gap_config_adv_data_raw`（30B）→ RAW_SET_COMPLETE_EVT → `esp_ble_gap_start_advertising`（ADV_TYPE_IND、500-510ms、全信道） |
| ADV_COMPLETE 重播 | Bluedroid 无此事件；断连后走现有状态机重新 config+start，START 失败进 RetryWait |

广播原始数据 30B：`02 01 06` + `08 09 "FakePod"` + `11 07` + 16B 服务 UUID（沿用现有 `BLE_UUID128_INIT` 字节序），正好在 31B 上限内；以后再加字段必须挪到 scan response。

新增状态字段：`gatts_if`、`handles[6]`、`conn_id`、`peer_bda`、`encrypted`、`bond_retry_done`、`service_started`、`adv_starting`；全部继续由现有 `g_lock` 保护。

## 改动 4：生命周期

- `start_stack()`：controller init/enable(BTDM) → bluedroid init/enable → 注册 GAP/GATTS 回调 → SMP 参数 → `esp_bt_gap_set_scan_mode(NON_CONNECTABLE, NON_DISCOVERABLE)`（Classic 无 profile，避免影子设备）→ 设备名/外观 → `app_register`；REG_EVT 后建表、启动服务、配广播、开广播。状态仍停留在 Starting，只有 ADV START_COMPLETE 成功才置 Advertising；失败置 RetryWait（2s）。原有 Starting 5 秒超时兜底保留。
- `stop_stack()`：`esp_ble_gatts_app_unregister` → 等 UNREG_EVT（≤1s）→ `esp_bluedroid_disable/deinit` → `esp_bt_controller_disable/deinit`；任一步失败返回错误，由既有 transition 任务进 RetryWait 重试；清空 conn/订阅/加密/句柄状态。
- 现有 `ble_remote_service_update()` 状态机（Starting/Advertising/Connected/Stopping/RetryWait）逻辑保持不变，只替换 Controller 分支。

## 改动 5：设置与构建

- [settings_app.cpp](file:///D:/ESP32/FakePod_S31/src/app/settings_app.cpp#L1568-L1573)：模式循环在 HID 不可用时跳过 Controller（Off→PhoneRemote→Off）；可用头文件新增的 `ble_remote_service_controller_supported()` 判断，避免 UI 选到不可用模式。
- [src/CMakeLists.txt](file:///D:/ESP32/FakePod_S31/src/CMakeLists.txt#L169-L195)：第一阶段从 REQUIRES 删除 `esp_hid`（HID 恢复时再加回）。
- `ble_remote_service.h` 增加 `controller_supported()` 声明；其余公共 API 签名不变（外部调用点无需改）。

## 验证

1. 构建：删除 `sdkconfig.fakepod_s31` 后 `pio run -e fakepod_s31`；确认配置已切换、编译零错误，记录 Flash 占用与内部 RAM 变化。
2. 烧录：用户关闭串口工具后 `pio run -e fakepod_s31 -t upload`，`pio device monitor -e fakepod_s31`。
3. 日志检查点：controller/host 初始化模式、GATTS REG_EVT 与 handles、START_EVT、RAW_SET_COMPLETE、ADV_START_COMPLETE status、连接 conn_id、AUTH_CMPL 结果、CCCD 订阅、断连 reason、UNREG/deinit；启动后与播放中的内部堆余量。
4. 手机回归（Android + iOS 各一轮）：配对/重连、128 位服务发现、全部命令码、20B Event Notify、metadata 分片、0x31 曲库、0x18 seek、0x20 Wi-Fi 配网、READ 偏移、蓝牙开关多次、Wi-Fi↔BLE 切换。

## 风险与回退

- 内部 RAM：Bluedroid 不能放 PSRAM，Classic 控制器另有常驻缓冲。若实测余量不足，先回退 `BTDM_CTRL_MODE_BLE_ONLY=y`（仍是 Bluedroid），第二阶段再评估。
- 配对：bond 存储从 NimBLE 换到 Bluedroid，手机需「忘记设备」后重新配对；必要时清一次 NVS。
- Service change：先用 AUTO；若首连订阅异常，改 MANUAL 并在连接后主动发 indication。
- 广播：31B 上限已用满（30B），后续字段须走 scan response。
- 本阶段不触碰音频路径；96k APLL 实机出声仍待独立确认。
