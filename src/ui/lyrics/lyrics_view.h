#pragma once

#include "lvgl.h"

// P1.4.3：真实 LRC 平滑歌词页 + 底部轻量控制 Overlay。
// 5 个逻辑歌词槽位；当前句最多两行并使用彩色高亮；切句时只移动 5 个 label 做短时纵向缓动。
// Overlay 只覆盖底部，提供进度/时间/上一曲/播放暂停/下一曲；显示期间上下滑预览音量，松手提交一次。
// 页面只消费 LyricsService + AudioStateSnapshot，绝不直接访问 decoder/I2S。
void lyrics_view_create(lv_obj_t *screen);
void lyrics_view_open();
void lyrics_view_close();
// PreserveBackground 专用：只隐藏页面和暂停 UI timer，不清空已解析歌词/当前行状态。
void lyrics_view_suspend_for_app_switch();
void lyrics_view_resume_after_app_switch();
bool lyrics_view_is_visible();

// 由播放器唯一 GestureRouter 消费点调用；Overlay 显示时先处理纵向音量，页面级横滑由调用方锁定。
bool lyrics_view_overlay_is_visible();
bool lyrics_view_process_overlay_interaction();

// R.30 性能审计只读状态。
bool lyrics_view_motion_is_active();
