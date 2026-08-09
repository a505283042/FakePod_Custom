#include "spectrum_view.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio_service.h"
#include "audio/audio_types.h"
#include "esp_log.h"
#include "font/font_manager.h"
#include "media/library/media_catalog_v2.h"
#include "player/player_playlist.h"
#include "input/touch_input.h"
#include "ui_common.h"

namespace
{
static const char *TAG = "频谱界面";

constexpr uint32_t SPECTRUM_FRAME_MS = 66U; // 约 15 FPS；先验证 UI 负载，不追求高刷新率。
constexpr uint32_t SPECTRUM_TOUCH_YIELD_MS = 120U; // 触摸期间+松手后短暂跳帧，把交互预算让给 LVGL。
constexpr uint8_t SPECTRUM_BAR_COUNT = 24U;
constexpr int16_t SPECTRUM_BAR_W = 10;
constexpr int16_t SPECTRUM_BAR_GAP = 5;
constexpr int16_t SPECTRUM_AREA_TOP = 126;
constexpr int16_t SPECTRUM_AREA_BOTTOM = 356;
constexpr int16_t SPECTRUM_MIN_H = 8;
constexpr int16_t SPECTRUM_MAX_H = SPECTRUM_AREA_BOTTOM - SPECTRUM_AREA_TOP;
constexpr uint8_t SPECTRUM_TARGET_HOLD_FRAMES = 3U;

lv_obj_t *g_root = nullptr;
lv_obj_t *g_title = nullptr;
lv_obj_t *g_artist = nullptr;
lv_obj_t *g_time = nullptr;
lv_obj_t *g_bars[SPECTRUM_BAR_COUNT] = {};
lv_timer_t *g_timer = nullptr;
bool g_visible = false;
uint32_t g_rng = 0x5A17D3C9U;
uint32_t g_frame = 0U;
uint32_t g_last_track = UINT32_MAX;
uint16_t g_bar_height[SPECTRUM_BAR_COUNT] = {};
uint16_t g_bar_target[SPECTRUM_BAR_COUNT] = {};

// 让低频/中低频略高，高频逐渐收敛；只是 P1.5.1 的视觉假数据，不代表真实频谱。
constexpr uint8_t kEnvelope[SPECTRUM_BAR_COUNT] = {
    64, 78, 92, 108, 122, 136, 148, 158,
    166, 174, 180, 184, 180, 174, 166, 156,
    146, 134, 122, 108, 94, 82, 70, 60,
};

static uint32_t spectrum_rng_next()
{
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x == 0U ? 0x5A17D3C9U : x;
    return g_rng;
}

static void spectrum_format_time(uint64_t ms, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0U) {
        return;
    }
    const uint64_t seconds = ms / 1000ULL;
    const uint64_t minutes = seconds / 60ULL;
    snprintf(out, out_size, "%llu:%02llu",
        static_cast<unsigned long long>(minutes),
        static_cast<unsigned long long>(seconds % 60ULL));
}

static lv_obj_t *spectrum_create_label(
    lv_obj_t *parent,
    const char *text,
    lv_color_t color,
    int32_t width,
    int32_t height)
{
    lv_obj_t *label = lv_label_create(parent);
    ui_common_lock_object(label);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, font_manager_get_ui_font(), 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(label, 0, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text != nullptr ? text : "");
    return label;
}

static void spectrum_refresh_header(uint32_t track_index)
{
    if (g_title == nullptr || g_artist == nullptr || track_index == g_last_track) {
        return;
    }

    const char *title = "未选择歌曲";
    const char *artist = "";
    MediaTrackViewV2 view = {};
    if (track_index != UINT32_MAX && media_catalog_v2_get_track_view(track_index, &view)) {
        if (view.title != nullptr && view.title[0] != '\0') {
            title = view.title;
        }
        if (view.artist != nullptr && view.artist[0] != '\0') {
            artist = view.artist;
        }
    }

    lv_label_set_text(g_title, title);
    lv_label_set_text(g_artist, artist);
    g_last_track = track_index;
}

static uint32_t spectrum_current_track(const AudioStateSnapshot &audio)
{
    if (audio.track_index != UINT32_MAX) {
        return audio.track_index;
    }
    size_t selected = 0U;
    return player_playlist_get_track_index(&selected)
        ? static_cast<uint32_t>(selected)
        : UINT32_MAX;
}

static void spectrum_choose_targets(bool playing)
{
    for (uint8_t i = 0U; i < SPECTRUM_BAR_COUNT; ++i) {
        if (!playing) {
            g_bar_target[i] = SPECTRUM_MIN_H;
            continue;
        }
        const uint32_t random = spectrum_rng_next();
        const uint32_t envelope = kEnvelope[i];
        const uint32_t jitter = 32U + (random % 192U); // 32..223
        uint32_t target = SPECTRUM_MIN_H + (envelope * jitter) / 255U;
        if (target > static_cast<uint32_t>(SPECTRUM_MAX_H)) {
            target = SPECTRUM_MAX_H;
        }
        g_bar_target[i] = static_cast<uint16_t>(target);
    }
}

static void spectrum_update_bars(bool playing)
{
    if ((g_frame % SPECTRUM_TARGET_HOLD_FRAMES) == 0U) {
        spectrum_choose_targets(playing);
    }

    for (uint8_t i = 0U; i < SPECTRUM_BAR_COUNT; ++i) {
        int32_t current = g_bar_height[i];
        const int32_t target = g_bar_target[i];
        if (target > current) {
            // 上升快一点。
            current += (target - current + 1) / 2;
        } else {
            // 回落慢一点，模拟 peak decay。
            current += (target - current) / 5;
        }
        if (current < SPECTRUM_MIN_H) {
            current = SPECTRUM_MIN_H;
        }
        if (current > SPECTRUM_MAX_H) {
            current = SPECTRUM_MAX_H;
        }

        if (static_cast<uint16_t>(current) == g_bar_height[i] || g_bars[i] == nullptr) {
            continue;
        }
        g_bar_height[i] = static_cast<uint16_t>(current);
        lv_obj_set_height(g_bars[i], current);
        lv_obj_set_y(g_bars[i], SPECTRUM_AREA_BOTTOM - current);
    }
}

static void spectrum_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!g_visible || g_root == nullptr || lv_obj_has_flag(g_root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    // P1.5R.1.2：频谱属于装饰动画。只要 TouchInputTask 检测到手指正在操作，
    // 或刚松手 120ms，本帧直接丢弃；不改对象、不触发新的 invalidate/flush。
    if (ui_touch_input_recent_activity(SPECTRUM_TOUCH_YIELD_MS)) {
        return;
    }

    AudioStateSnapshot audio = {};
    if (!audio_service_get_snapshot(&audio)) {
        return;
    }

    spectrum_refresh_header(spectrum_current_track(audio));

    const bool playing = audio.state == AudioPlaybackState::Playing;
    spectrum_update_bars(playing);
    ++g_frame;

    if ((g_frame % 4U) == 0U && g_time != nullptr) {
        char current[20] = {};
        spectrum_format_time(audio.position_ms, current, sizeof(current));
        lv_label_set_text(g_time, current);
    }
}
} // namespace

void spectrum_view_create(lv_obj_t *screen)
{
    if (screen == nullptr || g_root != nullptr) {
        return;
    }

    g_root = lv_obj_create(screen);
    ui_common_lock_object(g_root);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_size(g_root, 460, 460);
    lv_obj_set_style_radius(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x05080D), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_shadow_width(g_root, 0, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);

    g_title = spectrum_create_label(g_root, "频谱", lv_color_hex(0xFFFFFF), 392, 34);
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 24);

    g_artist = spectrum_create_label(g_root, "", lv_color_hex(0x8995A6), 392, 30);
    lv_obj_align(g_artist, LV_ALIGN_TOP_MID, 0, 60);

    constexpr int32_t bars_total_w =
        SPECTRUM_BAR_COUNT * SPECTRUM_BAR_W + (SPECTRUM_BAR_COUNT - 1) * SPECTRUM_BAR_GAP;
    const int32_t bars_left = (460 - bars_total_w) / 2;
    for (uint8_t i = 0U; i < SPECTRUM_BAR_COUNT; ++i) {
        lv_obj_t *bar = lv_obj_create(g_root);
        g_bars[i] = bar;
        ui_common_lock_object(bar);
        lv_obj_set_pos(
            bar,
            bars_left + static_cast<int32_t>(i) * (SPECTRUM_BAR_W + SPECTRUM_BAR_GAP),
            SPECTRUM_AREA_BOTTOM - SPECTRUM_MIN_H);
        lv_obj_set_size(bar, SPECTRUM_BAR_W, SPECTRUM_MIN_H);
        lv_obj_set_style_radius(bar, 4, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x62B7F2), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_shadow_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        g_bar_height[i] = SPECTRUM_MIN_H;
        g_bar_target[i] = SPECTRUM_MIN_H;
    }

    g_time = spectrum_create_label(g_root, "0:00", lv_color_hex(0x7E8998), 180, 30);
    lv_obj_align(g_time, LV_ALIGN_BOTTOM_MID, 0, -38);

    lv_obj_t *hint = spectrum_create_label(
        g_root,
        "P1.5.1 · DEMO",
        lv_color_hex(0x516070),
        220,
        26);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    g_timer = lv_timer_create(spectrum_timer_cb, SPECTRUM_FRAME_MS, nullptr);
    if (g_timer != nullptr) {
        lv_timer_pause(g_timer);
    }

    ESP_LOGI(TAG,
        "P1.5R.1.2 频谱页：假数据=%u柱 刷新=%ums；触摸期间+%ums跳帧；隐藏时timer暂停；不读取PCM/不做FFT",
        static_cast<unsigned>(SPECTRUM_BAR_COUNT),
        static_cast<unsigned>(SPECTRUM_FRAME_MS),
        static_cast<unsigned>(SPECTRUM_TOUCH_YIELD_MS));
}

void spectrum_view_open()
{
    if (g_root == nullptr || g_visible) {
        return;
    }

    g_visible = true;
    g_last_track = UINT32_MAX;
    g_frame = 0U;
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);

    AudioStateSnapshot audio = {};
    if (audio_service_get_snapshot(&audio)) {
        spectrum_refresh_header(spectrum_current_track(audio));
        spectrum_choose_targets(audio.state == AudioPlaybackState::Playing);
        char current[20] = {};
        spectrum_format_time(audio.position_ms, current, sizeof(current));
        lv_label_set_text(g_time, current);
    }
    if (g_timer != nullptr) {
        lv_timer_resume(g_timer);
    }
    ESP_LOGI(TAG, "打开频谱页：P1.5R.1.2 假数据 + 四页Interaction QoS验证");
}

void spectrum_view_close()
{
    if (g_root == nullptr || !g_visible) {
        return;
    }

    g_visible = false;
    if (g_timer != nullptr) {
        lv_timer_pause(g_timer);
    }
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "关闭频谱页，返回封面主页");
}

bool spectrum_view_is_visible()
{
    return g_visible && g_root != nullptr && !lv_obj_has_flag(g_root, LV_OBJ_FLAG_HIDDEN);
}
