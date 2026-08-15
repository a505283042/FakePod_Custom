# FakePod Ebook Reader V1 Final

正式版本：**P1.5.3.2R.39.6.5 — ebook-reader-v1-final**

本版本冻结 Ebook Reader V1。后续默认不再增加 Ebook V1 功能；仅接受影响正确性、稳定性或硬件兼容性的 bugfix。字号/主题、GBK/GB18030、章节导航、搜索、多书签等进入 V2 backlog。

## V1 冻结范围

- 书库根目录固定 `/sdcard/BOOKS`，支持子目录。
- Browser 仅显示目录和 `.txt`；目录优先、名称大小写不敏感排序。
- Browser 数据层采用 PSRAM `DirectoryEntryIndex + StringPool`，不再有 48 项硬上限；EntryIndex 固定 16B/项并按需扩容。
- Browser UI 固定复用 5 个 Virtual Row，不使用 LVGL native continuous scroll / momentum / elastic；纵向手势按 4 项步进，避免大对象列表持续 redraw 压垮后台 FLAC。
- 编码支持 UTF-8 / UTF-8 BOM；UTF-16 与非法 UTF-8 明确拒绝。GBK/GB18030 不属于 V1。
- Reader 默认进入带标签视图（9 行），可切全屏视图。
- 两种视图各自维护独立 Canonical PageIndex，通过同一个正文 byte anchor 映射，切换视图不制造人工半页。
- 阅读位置/书签保存在 NVS；退出 TXT 时保存正文 byte anchor。
- 长书使用 cooperative PageIndex Builder V2、TF PageIndex Cache V1、Dual View Shadow Index。
- Browser 返回与底边上滑 Launcher 走共享 GestureRouter。
- Ebook 不新增 FreeRTOS Task；TF I/O 统一走 `StorageSdLockGuard`，后台目录扫描/PageIndex/Cache 在 SD-FLAC ring <90% 时主动让路。
- LVGL builtin TLSF pool 固定 128KB，backing 位于 PSRAM。
- FLAC 保留 bounded starvation recovery 作为异常保险丝；正常 Ebook 浏览/阅读不应依赖该恢复路径。

## Final 实机封板证据（YCB）

R.39.6.4.x RC 阶段已完成并验证以下关键组合：

1. **LVGL 内存**：39 项目录在 Virtual Browser 下不再创建 117+ 个 Row 子对象；目录绑定后 LVGL pool 维持约 42% 使用率，未复现 `lv_draw_add_task` StoreProhibited。
2. **Directory Index**：39 项实机索引为 `624B`（39×16B）+ `2139B` StringPool；host 压力测试 1002 个有效项目无 48 项截断、无越界/泄漏。
3. **Virtual Browser + 后台 FLAC**：连续快速前后浏览 39 项时，48kHz/24bit FLAC ring 最低约 80%，Emergency=0，未出现 starvation / AUDIO_FAULT。
4. **大 TXT Reader**：约 2.55MB UTF-8 小说正常打开并连续翻页。
5. **双视图**：带标签 ↔ 全屏切换后，Canonical PageIndex/anchor 连续工作。
6. **Shadow Index**：翻页后另一视图索引按 idle window 追平，未阻塞前台 Reader。
7. **书签**：退出 TXT 时成功保存正文 offset，并按 QoS 规则提交/延后 PageIndex checkpoint。
8. **后台音乐**：目录浏览、打开大书、连续翻页、视图切换期间 AudioTask 保持后台播放；未出现 ring=0。
9. **Launcher**：公共圆环可从 Ebook 呼出并返回 Music；观察到一次短时 FLAC Emergency（约 410ms 后恢复），无 starvation/AUDIO_FAULT，保留为运行期 watchpoint。

## Release 运行规则

- `AB_TRACE / AB_MARK` 为 RC 临时 A/B 诊断，R.39.6.5 已移除。
- FLAC 每次 source read 的 `esp_timer_get_time()` 计时仅在 `APP_DIAG_FLAC_PERFORMANCE`/STRESS 打开时编译，Release 热路径不再承担 A/B 诊断计时开销。
- RC 阶段用于定位 LVGL pool 的 `lv_mem_monitor()` Ebook 日志已移除。
- Emergency/恢复、AUDIO_FAULT、TF/Reader 错误等必要 W/E 日志继续保留。

## Ebook V2 Backlog

- GBK / GB18030 / ANSI TXT 转码。
- 字号、行距、页边距、主题。
- 多书签 / 书签列表。
- 章节目录与全文搜索。
- 更强的正文 fingerprint，用于识别“同路径但内容已替换”的 TXT。
- 更完整的中文避头尾（kinsoku）与英文单词级换行。
- 若未来需要连续像素滚动手感，应基于真正的虚拟化 renderer 单独设计，不恢复多 Row LVGL native scroll。
