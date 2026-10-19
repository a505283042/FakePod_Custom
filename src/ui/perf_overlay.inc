// Included by ui_manager.cpp; all callbacks execute under the LVGL lock.
#ifndef FAKEPOD_PERF_OVERLAY
#define FAKEPOD_PERF_OVERLAY 0
#endif
#if FAKEPOD_PERF_OVERLAY
#if !CONFIG_FREERTOS_USE_TRACE_FACILITY || !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS || !CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
#error "Use fakepod_overlay with sdkconfig.overlay.defaults for CPU statistics"
#endif
#include "perf_overlay_math.h"
static lv_obj_t *g_perf_overlay_label = nullptr;
static uint32_t g_perf_overlay_frames = 0;
static bool g_perf_overlay_rendered = false;
static uint32_t g_perf_overlay_idle[2] = {};
static int64_t g_perf_overlay_sample_us = 0;
static void ui_perf_overlay_display_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
        g_perf_overlay_rendered = true;
    } else if (lv_event_get_code(event) == LV_EVENT_REFR_READY) {
        if (g_perf_overlay_rendered) ++g_perf_overlay_frames;
        g_perf_overlay_rendered = false;
    }
}
static void ui_perf_overlay_timer_cb(lv_timer_t *)
{
    if (!g_perf_overlay_label) return;
    const int64_t now = esp_timer_get_time();
    uint32_t idle[2] = {};
    for (int core = 0; core < 2; ++core) {
        TaskStatus_t status = {};
        // Query only the two idle tasks; do not enumerate tasks or scan stacks.
        vTaskGetInfo(xTaskGetIdleTaskHandleForCore(core), &status, pdFALSE, eInvalid);
        idle[core] = static_cast<uint32_t>(status.ulRunTimeCounter);
    }
    const uint64_t elapsed = static_cast<uint64_t>(now - g_perf_overlay_sample_us);
    const bool normal = screen_lock_simple_get_power() == ScreenPowerNormal;
    if (!normal) {
        if (!lv_obj_has_flag(g_perf_overlay_label, LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(g_perf_overlay_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(g_perf_overlay_label, LV_OBJ_FLAG_HIDDEN);
        if (g_perf_overlay_sample_us != 0 && elapsed > 0 && elapsed < UINT32_MAX) {
            const unsigned cpu0 = fakepod_perf::busy_percent(idle[0] - g_perf_overlay_idle[0], elapsed);
            const unsigned cpu1 = fakepod_perf::busy_percent(idle[1] - g_perf_overlay_idle[1], elapsed);
            const unsigned fps = fakepod_perf::fps_tenths(g_perf_overlay_frames, elapsed);
            char text[80];
            snprintf(text, sizeof(text), "CPU0 %3u%%  CPU1 %3u%%\nUI %u.%u FPS", cpu0, cpu1, fps / 10U, fps % 10U);
            // Avoid invalidating an unchanged label.
            if (strcmp(lv_label_get_text(g_perf_overlay_label), text) != 0)
                lv_label_set_text(g_perf_overlay_label, text);
        }
    }
    g_perf_overlay_sample_us = now;
    g_perf_overlay_idle[0] = idle[0];
    g_perf_overlay_idle[1] = idle[1];
    g_perf_overlay_frames = 0;
}
static void ui_perf_overlay_create()
{
    if (g_perf_overlay_label) return;
    g_perf_overlay_label = lv_label_create(lv_layer_sys());
    if (!g_perf_overlay_label) return;
    ui_common_lock_object(g_perf_overlay_label);
    lv_obj_remove_flag(g_perf_overlay_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(g_perf_overlay_label, lv_font_default(), 0);
    lv_obj_set_style_text_color(g_perf_overlay_label, lv_color_hex(0xA8F0C0), 0);
    lv_obj_set_style_bg_color(g_perf_overlay_label, lv_color_hex(0x101810), 0);
    lv_obj_set_style_bg_opa(g_perf_overlay_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_perf_overlay_label, 4, 0);
    lv_label_set_text(g_perf_overlay_label, "CPU0 ---  CPU1 ---\nUI --.- FPS");
    lv_obj_align(g_perf_overlay_label, LV_ALIGN_TOP_RIGHT, -12, 58);
    lv_timer_t *timer = lv_timer_create(ui_perf_overlay_timer_cb, 1000, nullptr);
    if (!timer) {
        lv_obj_delete(g_perf_overlay_label);
        g_perf_overlay_label = nullptr;
        return;
    }
    lv_display_add_event_cb(g_display, ui_perf_overlay_display_cb, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(g_display, ui_perf_overlay_display_cb, LV_EVENT_REFR_READY, nullptr);
    ui_perf_overlay_timer_cb(timer);
}
#else
static inline void ui_perf_overlay_create() {}
#endif
