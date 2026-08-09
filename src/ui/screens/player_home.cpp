#include "player_home.h"

#include <stdio.h>

#include "esp_log.h"
#include "audio_service.h"
#include "artwork/now_playing_artwork.h"
#include "board_pins.h"
#include "font/font_manager.h"
#include "media_library.h"
#include "player_control.h"
#include "player_state.h"
#include "ui_common.h"

static const char *TAG = "首页";

// UI Reset P1.2.5：播放器默认只显示全屏封面。封面后台只预处理 normal RGB565，
// 460x460 RGB565 surface；单击时只切换 surface 并显示透明控制层。只有预处理失败的兼容
// 回退路径才使用全屏 alpha 黑层。AudioTask / Player Transport 仍沿用既有实现。
static constexpr int32_t kProgressScale = 10000;
static constexpr uint32_t kOverlayTimeoutMs = 5000U;
static constexpr lv_opa_t kOverlayDimOpacity = 150U;

static lv_obj_t *g_overlay = nullptr;
static lv_obj_t *g_overlay_backdrop = nullptr;
static lv_timer_t *g_overlay_timer = nullptr;
static bool g_overlay_visible = false;
static bool g_overlay_fast_dim = false;
static bool g_overlay_dim_path_valid = false;

static lv_obj_t *g_title = nullptr;
static lv_obj_t *g_track_info = nullptr;
static lv_obj_t *g_play_symbol = nullptr;
static lv_obj_t *g_progress = nullptr;
static lv_obj_t *g_current_time = nullptr;
static lv_obj_t *g_total_time = nullptr;
static lv_obj_t *g_loop_label = nullptr;
static lv_obj_t *g_volume_label = nullptr;
static lv_obj_t *g_volume_slider = nullptr;

// Stage 11.2：进度条使用 0~10000 的归一化范围，避免把超长音频毫秒数直接塞进 LVGL int32_t range。
// 拖动期间只做 UI 本地预览；松手时才向 Player 提交一次 Seek。
static bool g_progress_dragging = false;
static bool g_progress_seek_pending = false;
static uint64_t g_progress_total_ms = 0;
static uint64_t g_progress_preview_ms = 0;
static uint32_t g_progress_track_index = UINT32_MAX;
static uint32_t g_progress_playback_revision = 0;
static uint32_t g_progress_seek_base_revision = 0;
static uint32_t g_progress_seek_started_tick = 0;

static bool g_volume_dragging = false;
static uint32_t g_last_audio_state_revision = UINT32_MAX;

static lv_obj_t *player_home_create_label(
    lv_obj_t *parent,
    const char *text,
    lv_color_t color,
    const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    ui_common_lock_object(label);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font != nullptr ? font : lv_font_default(), 0);
    return label;
}

static lv_obj_t *player_home_create_round_button(lv_obj_t *parent, int32_t size, const char *symbol)
{
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(button, 36, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);

    lv_obj_t *label = player_home_create_label(
        button, symbol, lv_color_hex(0xFFFFFF), lv_font_default());
    lv_obj_center(label);
    return button;
}

static lv_obj_t *player_home_create_pill_button(
    lv_obj_t *parent,
    int32_t width,
    int32_t height,
    const char *text,
    lv_obj_t **out_label)
{
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(button, 28, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);

    lv_obj_t *label = player_home_create_label(
        button, text, lv_color_hex(0xF5F7FA), font_manager_get_ui_font());
    lv_obj_center(label);
    if (out_label != nullptr) {
        *out_label = label;
    }
    return button;
}

static void player_home_overlay_arm_timeout()
{
    if (g_overlay_timer == nullptr || !g_overlay_visible) {
        return;
    }
    lv_timer_set_period(g_overlay_timer, kOverlayTimeoutMs);
    lv_timer_reset(g_overlay_timer);
    lv_timer_resume(g_overlay_timer);
}

static void player_home_overlay_apply_dim_path()
{
    if (g_overlay_backdrop == nullptr) {
        return;
    }

    // P1.2.5：固定使用 alpha 黑层。底图在正常路径已是最终 460x460 RGB565，
    // 因而 Overlay 出现只做一次轻量合成，不会重新解码/缩放封面。
    // now_playing_artwork_set_dimmed() 仅保留兼容接口，当前返回 false。
    const bool fast_dim = now_playing_artwork_set_dimmed(g_overlay_visible);
    if (!g_overlay_dim_path_valid || g_overlay_fast_dim != fast_dim) {
        g_overlay_fast_dim = fast_dim;
        g_overlay_dim_path_valid = true;
        const lv_opa_t backdrop_opa =
            fast_dim ? static_cast<lv_opa_t>(LV_OPA_TRANSP) : kOverlayDimOpacity;
        lv_obj_set_style_bg_opa(
            g_overlay_backdrop,
            backdrop_opa,
            0);
    }
}

static void player_home_overlay_show()
{
    if (g_overlay == nullptr) {
        return;
    }

    if (!g_overlay_visible) {
        g_overlay_visible = true;
        g_overlay_dim_path_valid = false;
        player_home_overlay_apply_dim_path();
        lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    player_home_overlay_arm_timeout();
}

static void player_home_overlay_hide()
{
    if (g_overlay == nullptr || !g_overlay_visible) {
        return;
    }
    g_overlay_visible = false;
    if (g_overlay_timer != nullptr) {
        lv_timer_pause(g_overlay_timer);
    }
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    // 保留接口调用，P1.2.5 下底图始终是 normal RGB565。
    now_playing_artwork_set_dimmed(false);
    g_overlay_fast_dim = false;
    g_overlay_dim_path_valid = false;
}

static void player_home_overlay_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_progress_dragging || g_progress_seek_pending || g_volume_dragging) {
        player_home_overlay_arm_timeout();
        return;
    }
    player_home_overlay_hide();
}

static void player_home_screen_tap_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        player_home_overlay_show();
    }
}

static void player_home_overlay_backdrop_tap_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !g_overlay_visible) {
        return;
    }
    // 暗色背景是独立的最底层命中面。按钮和 Slider 均位于它上方，因此空白处
    // 才会进入这里，控制控件的触摸不会再被整屏 Overlay 抢走。
    player_home_overlay_hide();
}

static uint64_t player_home_snapshot_total_ms(const AudioStateSnapshot &snapshot)
{
    if (snapshot.sample_rate_hz == 0U || snapshot.total_frames == 0U) {
        return 0U;
    }
    return (snapshot.total_frames * 1000ULL) / snapshot.sample_rate_hz;
}

static int32_t player_home_progress_value_from_ms(uint64_t position_ms, uint64_t total_ms)
{
    if (total_ms == 0U) {
        return 0;
    }
    if (position_ms >= total_ms) {
        return kProgressScale;
    }
    return static_cast<int32_t>((position_ms * static_cast<uint64_t>(kProgressScale)) / total_ms);
}

static uint64_t player_home_progress_ms_from_value(int32_t value, uint64_t total_ms)
{
    if (total_ms == 0U || value <= 0) {
        return 0U;
    }
    if (value >= kProgressScale) {
        return total_ms > 0U ? total_ms - 1U : 0U;
    }
    return (total_ms * static_cast<uint64_t>(value)) / static_cast<uint64_t>(kProgressScale);
}

static void player_home_format_time(uint64_t ms, char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U) {
        return;
    }
    const uint64_t total_seconds = ms / 1000ULL;
    const uint64_t hours = total_seconds / 3600ULL;
    const uint64_t minutes = (total_seconds / 60ULL) % 60ULL;
    const uint64_t seconds = total_seconds % 60ULL;
    if (hours > 0U) {
        snprintf(buffer, buffer_size, "%llu:%02llu:%02llu",
            static_cast<unsigned long long>(hours),
            static_cast<unsigned long long>(minutes),
            static_cast<unsigned long long>(seconds));
    } else {
        snprintf(buffer, buffer_size, "%llu:%02llu",
            static_cast<unsigned long long>(total_seconds / 60ULL),
            static_cast<unsigned long long>(seconds));
    }
}

static void player_home_update_time_labels(uint64_t position_ms, uint64_t total_ms)
{
    char current[24] = {};
    char total[24] = {};
    player_home_format_time(position_ms, current, sizeof(current));
    player_home_format_time(total_ms, total, sizeof(total));
    if (g_current_time != nullptr) {
        lv_label_set_text(g_current_time, current);
    }
    if (g_total_time != nullptr) {
        lv_label_set_text(g_total_time, total);
    }
}

static bool player_home_snapshot_can_scrub(const AudioStateSnapshot &snapshot)
{
    if (!snapshot.ready || !snapshot.seek_supported || snapshot.track_index == UINT32_MAX) {
        return false;
    }
    if (snapshot.track_index != player_state_get_index()) {
        return false;
    }
    if (snapshot.sample_rate_hz == 0U || snapshot.total_frames == 0U) {
        return false;
    }
    return snapshot.state == AudioPlaybackState::Playing ||
        snapshot.state == AudioPlaybackState::Paused ||
        snapshot.state == AudioPlaybackState::Seeking;
}

static void player_home_progress_set_enabled(bool enabled)
{
    if (g_progress == nullptr) {
        return;
    }
    if (enabled) {
        lv_obj_remove_state(g_progress, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(g_progress, LV_STATE_DISABLED);
    }
}

static void player_home_cancel_progress_interaction()
{
    g_progress_dragging = false;
    g_progress_seek_pending = false;
    g_progress_total_ms = 0U;
    g_progress_preview_ms = 0U;
    g_progress_track_index = UINT32_MAX;
    g_progress_playback_revision = 0U;
    g_progress_seek_base_revision = 0U;
    g_progress_seek_started_tick = 0U;
}

static void player_home_progress_sync(const AudioStateSnapshot &snapshot)
{
    if (g_progress == nullptr) {
        return;
    }

    const uint64_t total_ms = player_home_snapshot_total_ms(snapshot);
    const bool same_track = snapshot.track_index != UINT32_MAX &&
        snapshot.track_index == player_state_get_index();

    if (g_progress_dragging &&
        (!same_track || snapshot.track_index != g_progress_track_index ||
         snapshot.playback_revision != g_progress_playback_revision)) {
        player_home_cancel_progress_interaction();
    }

    if (g_progress_seek_pending) {
        const bool request_context_stale = !same_track ||
            snapshot.track_index != g_progress_track_index ||
            snapshot.playback_revision != g_progress_playback_revision;
        const bool seek_committed = snapshot.seek_revision != g_progress_seek_base_revision;
        const bool seek_failed = snapshot.state == AudioPlaybackState::Error;
        const bool seek_wait_expired = snapshot.state != AudioPlaybackState::Seeking &&
            lv_tick_elaps(g_progress_seek_started_tick) > 10000U;
        if (request_context_stale || seek_committed || seek_failed || seek_wait_expired) {
            g_progress_seek_pending = false;
        }
    }

    player_home_progress_set_enabled(player_home_snapshot_can_scrub(snapshot));

    if (g_progress_dragging || g_progress_seek_pending) {
        player_home_update_time_labels(g_progress_preview_ms, g_progress_total_ms);
        return;
    }

    if (!same_track || total_ms == 0U) {
        lv_slider_set_value(g_progress, 0, LV_ANIM_OFF);
        player_home_update_time_labels(0U, total_ms);
        return;
    }

    lv_slider_set_value(
        g_progress,
        player_home_progress_value_from_ms(snapshot.position_ms, total_ms),
        LV_ANIM_OFF);
    player_home_update_time_labels(snapshot.position_ms, total_ms);
}

static void player_home_progress_cb(lv_event_t *event)
{
    if (event == nullptr || g_progress == nullptr) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        player_home_overlay_show();
        AudioStateSnapshot snapshot = {};
        if (!audio_service_get_snapshot(&snapshot) || !player_home_snapshot_can_scrub(snapshot)) {
            return;
        }
        const uint64_t total_ms = player_home_snapshot_total_ms(snapshot);
        if (total_ms == 0U) {
            return;
        }
        g_progress_dragging = true;
        g_progress_seek_pending = false;
        g_progress_total_ms = total_ms;
        g_progress_track_index = snapshot.track_index;
        g_progress_playback_revision = snapshot.playback_revision;
        g_progress_seek_base_revision = snapshot.seek_revision;
        g_progress_preview_ms = player_home_progress_ms_from_value(
            lv_slider_get_value(g_progress), total_ms);
        player_home_update_time_labels(g_progress_preview_ms, g_progress_total_ms);
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED && g_progress_dragging) {
        g_progress_preview_ms = player_home_progress_ms_from_value(
            lv_slider_get_value(g_progress), g_progress_total_ms);
        player_home_update_time_labels(g_progress_preview_ms, g_progress_total_ms);
        player_home_overlay_arm_timeout();
        return;
    }

    if (code == LV_EVENT_PRESS_LOST && g_progress_dragging) {
        player_home_cancel_progress_interaction();
        player_home_refresh();
        player_home_overlay_arm_timeout();
        return;
    }

    if (code != LV_EVENT_RELEASED || !g_progress_dragging) {
        return;
    }

    AudioStateSnapshot snapshot = {};
    const bool snapshot_ok = audio_service_get_snapshot(&snapshot);
    const bool context_ok = snapshot_ok && player_home_snapshot_can_scrub(snapshot) &&
        snapshot.track_index == g_progress_track_index &&
        snapshot.playback_revision == g_progress_playback_revision;
    if (!context_ok) {
        player_home_cancel_progress_interaction();
        if (snapshot_ok) {
            player_home_refresh();
        }
        player_home_overlay_arm_timeout();
        return;
    }

    g_progress_preview_ms = player_home_progress_ms_from_value(
        lv_slider_get_value(g_progress), g_progress_total_ms);
    g_progress_dragging = false;
    g_progress_seek_base_revision = snapshot.seek_revision;
    g_progress_seek_started_tick = lv_tick_get();

    if (player_control_seek_ms(g_progress_preview_ms)) {
        g_progress_seek_pending = true;
        player_home_update_time_labels(g_progress_preview_ms, g_progress_total_ms);
    } else {
        player_home_cancel_progress_interaction();
        player_home_refresh();
    }
    player_home_overlay_arm_timeout();
}

static const char *player_home_state_name(const AudioStateSnapshot *snapshot)
{
    if (snapshot == nullptr) {
        return "";
    }
    switch (snapshot->state) {
        case AudioPlaybackState::Playing: return "播放中";
        case AudioPlaybackState::Paused: return "已暂停";
        case AudioPlaybackState::Seeking: return "跳转中";
        case AudioPlaybackState::Finished: return "播放结束";
        case AudioPlaybackState::Error: return "播放错误";
        default: return "";
    }
}

static void player_home_refresh_track(const AudioStateSnapshot *audio_snapshot)
{
    if (g_title == nullptr || g_track_info == nullptr) {
        return;
    }

    const size_t library_count = media_library_get_count();
    const size_t list_count = player_state_get_list_count();
    if (library_count == 0U || list_count == 0U) {
        lv_label_set_text(g_title, "暂无歌曲");
        lv_label_set_text(g_track_info, "音乐库为空");
        return;
    }

    const size_t index = player_state_get_index();
    const size_t list_position = player_state_get_list_position();
    char title[512] = {};
    if (!media_library_copy_display_name(index, title, sizeof(title))) {
        snprintf(title, sizeof(title), "歌曲 %u", static_cast<unsigned>(index + 1U));
    }
    lv_label_set_text(g_title, title);

    const char *state_name = "";
    if (audio_snapshot != nullptr && audio_snapshot->track_index == index) {
        state_name = player_home_state_name(audio_snapshot);
    }

    if (state_name[0] != '\0') {
        lv_label_set_text_fmt(
            g_track_info,
            "%u / %u  ·  %s  ·  %s",
            static_cast<unsigned>(list_position + 1U),
            static_cast<unsigned>(list_count),
            media_format_name(player_state_get_format()),
            state_name);
    } else {
        lv_label_set_text_fmt(
            g_track_info,
            "%u / %u  ·  %s",
            static_cast<unsigned>(list_position + 1U),
            static_cast<unsigned>(list_count),
            media_format_name(player_state_get_format()));
    }
}

static void player_home_refresh_transport_controls(const AudioStateSnapshot *snapshot)
{
    if (g_loop_label != nullptr) {
        lv_label_set_text(g_loop_label,
            player_transport_loop_mode_name(player_control_get_loop_mode()));
    }

    if (snapshot != nullptr) {
        if (!g_volume_dragging && g_volume_slider != nullptr) {
            lv_slider_set_value(g_volume_slider, snapshot->volume_percent, LV_ANIM_OFF);
        }
        if (!g_volume_dragging && g_volume_label != nullptr) {
            if (snapshot->user_muted) {
                lv_label_set_text(g_volume_label, "静音");
            } else {
                lv_label_set_text_fmt(g_volume_label, "%u%%", static_cast<unsigned>(snapshot->volume_percent));
            }
        }
    }
}

static void player_home_apply_audio_snapshot(const AudioStateSnapshot &snapshot)
{
    if (g_play_symbol != nullptr) {
        lv_label_set_text(
            g_play_symbol,
            snapshot.state == AudioPlaybackState::Playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    player_home_progress_sync(snapshot);
    player_home_refresh_track(&snapshot);
    player_home_refresh_transport_controls(&snapshot);
}

static void player_home_artwork_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    now_playing_artwork_update();
    // 如果 Overlay 打开期间后台 surface 刚好准备完成，保持 alpha 黑层，不再切换第二张 Surface。
    if (g_overlay_visible) {
        player_home_overlay_apply_dim_path();
    }
}

static void player_home_audio_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot)) {
        return;
    }
    if (snapshot.state_revision == g_last_audio_state_revision) {
        return;
    }
    g_last_audio_state_revision = snapshot.state_revision;
    player_home_apply_audio_snapshot(snapshot);
}

static void player_home_prev_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    player_home_overlay_show();
    ESP_LOGI(TAG, "P1.2 控件命中：上一曲");
    if (player_control_previous()) {
        AudioStateSnapshot snapshot = {};
        audio_service_get_snapshot(&snapshot);
        player_home_apply_audio_snapshot(snapshot);
    }
}

static void player_home_next_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    player_home_overlay_show();
    ESP_LOGI(TAG, "P1.2 控件命中：下一曲");
    if (player_control_next()) {
        AudioStateSnapshot snapshot = {};
        audio_service_get_snapshot(&snapshot);
        player_home_apply_audio_snapshot(snapshot);
    }
}

static void player_home_play_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    player_home_overlay_show();
    ESP_LOGI(TAG, "P1.2 控件命中：播放/暂停");
    if (!player_control_toggle_play_pause()) {
        ESP_LOGW(TAG, "播放控制请求未能入队");
    }
}

static void player_home_loop_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    player_home_overlay_show();
    ESP_LOGI(TAG, "P1.2 控件命中：循环模式");
    player_control_cycle_loop_mode();
    AudioStateSnapshot snapshot = {};
    audio_service_get_snapshot(&snapshot);
    player_home_refresh_transport_controls(&snapshot);
}

static void player_home_mute_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    player_home_overlay_show();
    ESP_LOGI(TAG, "P1.2 控件命中：静音");
    if (!player_control_toggle_mute()) {
        ESP_LOGW(TAG, "静音切换请求未能入队");
    }
}

static void player_home_volume_cb(lv_event_t *event)
{
    if (event == nullptr || g_volume_slider == nullptr) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        g_volume_dragging = true;
        player_home_overlay_show();
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED && g_volume_dragging) {
        if (g_volume_label != nullptr) {
            lv_label_set_text_fmt(g_volume_label, "%ld%%", static_cast<long>(lv_slider_get_value(g_volume_slider)));
        }
        player_home_overlay_arm_timeout();
        return;
    }

    if (code == LV_EVENT_PRESS_LOST && g_volume_dragging) {
        g_volume_dragging = false;
        AudioStateSnapshot snapshot = {};
        if (audio_service_get_snapshot(&snapshot)) {
            player_home_refresh_transport_controls(&snapshot);
        }
        player_home_overlay_arm_timeout();
        return;
    }

    if (code != LV_EVENT_RELEASED || !g_volume_dragging) {
        return;
    }

    g_volume_dragging = false;
    const int32_t value = lv_slider_get_value(g_volume_slider);
    ESP_LOGI(TAG, "P1.2 控件命中：音量=%ld%%", static_cast<long>(value));
    if (!player_control_set_volume(static_cast<uint8_t>(value < 0 ? 0 : (value > 100 ? 100 : value)))) {
        ESP_LOGW(TAG, "设置音量请求未能入队");
    }
    player_home_overlay_arm_timeout();
}

void player_home_refresh()
{
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot)) {
        return;
    }
    player_home_apply_audio_snapshot(snapshot);
    now_playing_artwork_refresh_context();
    g_last_audio_state_revision = snapshot.state_revision;
}

void player_home_create(lv_obj_t *screen)
{
    if (screen == nullptr) {
        return;
    }

    player_home_cancel_progress_interaction();
    g_volume_dragging = false;
    g_overlay_visible = false;
    g_overlay_fast_dim = false;
    g_overlay_dim_path_valid = false;
    g_overlay_backdrop = nullptr;

    ui_common_lock_object(screen);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(screen, player_home_screen_tap_cb, LV_EVENT_CLICKED, nullptr);

    // 全屏封面是播放器唯一背景；正常路径由 CoverSurfaceTask 提供最终 460x460 RGB565。
    lv_obj_t *cover = nullptr;
    const int32_t cover_size =
        FAKEPOD_LCD_WIDTH > FAKEPOD_LCD_HEIGHT ? FAKEPOD_LCD_WIDTH : FAKEPOD_LCD_HEIGHT;
    const esp_err_t artwork_ret = now_playing_artwork_create(screen, cover_size, &cover);
    if (artwork_ret == ESP_OK && cover != nullptr) {
        lv_obj_center(cover);
        lv_obj_move_background(cover);
    } else {
        ESP_LOGW(TAG, "创建全屏封面失败，继续黑底运行：%s", esp_err_to_name(artwork_ret));
    }

    // P1.2：Overlay 本体只负责承载控件，本身透明且不可点击。backdrop 仍作为最底层
    // 点击命中面，但正常情况下背景透明；只有最终 surface 预处理失败时才启用 alpha 黑层。
    g_overlay = lv_obj_create(screen);
    ui_common_lock_object(g_overlay);
    lv_obj_set_pos(g_overlay, 0, 0);
    lv_obj_set_size(g_overlay, FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT);
    lv_obj_set_style_radius(g_overlay, 0, 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_overlay, 0, 0);
    lv_obj_set_style_shadow_width(g_overlay, 0, 0);
    lv_obj_set_style_pad_all(g_overlay, 0, 0);
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_CLICKABLE);

    g_overlay_backdrop = lv_obj_create(g_overlay);
    ui_common_lock_object(g_overlay_backdrop);
    lv_obj_set_pos(g_overlay_backdrop, 0, 0);
    lv_obj_set_size(g_overlay_backdrop, FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT);
    lv_obj_set_style_radius(g_overlay_backdrop, 0, 0);
    lv_obj_set_style_bg_color(g_overlay_backdrop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_overlay_backdrop, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_overlay_backdrop, 0, 0);
    lv_obj_set_style_shadow_width(g_overlay_backdrop, 0, 0);
    lv_obj_set_style_pad_all(g_overlay_backdrop, 0, 0);
    lv_obj_add_flag(g_overlay_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        g_overlay_backdrop, player_home_overlay_backdrop_tap_cb, LV_EVENT_CLICKED, nullptr);

    // 顶部只保留轻量状态：左侧循环模式；右侧区域预留给后续 BatteryService。
    lv_obj_t *loop = player_home_create_pill_button(g_overlay, 94, 36, "顺序", &g_loop_label);
    lv_obj_align(loop, LV_ALIGN_TOP_LEFT, 104, 40);
    lv_obj_add_event_cb(loop, player_home_loop_cb, LV_EVENT_CLICKED, nullptr);

    g_title = player_home_create_label(
        g_overlay, "", lv_color_hex(0xFFFFFF), font_manager_get_ui_font());
    lv_label_set_long_mode(g_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_title, 350, 34);
    lv_obj_set_style_text_align(g_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 92);

    g_track_info = player_home_create_label(
        g_overlay, "", lv_color_hex(0xD2D6DC), font_manager_get_ui_font());
    lv_label_set_long_mode(g_track_info, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_track_info, 360, 30);
    lv_obj_set_style_text_align(g_track_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_track_info, LV_ALIGN_TOP_MID, 0, 127);

    // 中部进度区继续复用 Stage 11.2 Seek 语义。
    g_progress = lv_slider_create(g_overlay);
    ui_common_lock_object(g_progress);
    lv_obj_add_flag(g_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g_progress, 326, 10);
    lv_obj_align(g_progress, LV_ALIGN_TOP_MID, 0, 205);
    lv_slider_set_range(g_progress, 0, kProgressScale);
    lv_slider_set_value(g_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(g_progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_progress, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_progress, 55, LV_PART_MAIN);
    lv_obj_set_style_radius(g_progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_progress, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_width(g_progress, 18, LV_PART_KNOB);
    lv_obj_set_style_height(g_progress, 18, LV_PART_KNOB);
    lv_obj_set_style_radius(g_progress, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(g_progress, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_COVER, LV_PART_KNOB);
    const lv_style_selector_t disabled_indicator =
        static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
    const lv_style_selector_t disabled_knob =
        static_cast<lv_style_selector_t>(LV_PART_KNOB) | static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_40, disabled_indicator);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_40, disabled_knob);
    lv_obj_add_event_cb(g_progress, player_home_progress_cb, LV_EVENT_ALL, nullptr);
    player_home_progress_set_enabled(false);

    g_current_time = player_home_create_label(
        g_overlay, "0:00", lv_color_hex(0xE7E9ED), font_manager_get_ui_font());
    lv_obj_set_size(g_current_time, 100, 28);
    lv_obj_set_style_text_align(g_current_time, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(g_current_time, LV_ALIGN_TOP_MID, -113, 220);

    g_total_time = player_home_create_label(
        g_overlay, "0:00", lv_color_hex(0xE7E9ED), font_manager_get_ui_font());
    lv_obj_set_size(g_total_time, 100, 28);
    lv_obj_set_style_text_align(g_total_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_total_time, LV_ALIGN_TOP_MID, 113, 220);

    // 播放控制区。
    lv_obj_t *prev = player_home_create_round_button(g_overlay, 60, LV_SYMBOL_PREV);
    lv_obj_align(prev, LV_ALIGN_CENTER, -88, 70);
    lv_obj_add_event_cb(prev, player_home_prev_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *play = player_home_create_round_button(g_overlay, 78, LV_SYMBOL_PLAY);
    lv_obj_align(play, LV_ALIGN_CENTER, 0, 70);
    lv_obj_set_style_bg_opa(play, 225, 0);
    g_play_symbol = lv_obj_get_child(play, 0);
    if (g_play_symbol != nullptr) {
        lv_obj_set_style_text_color(g_play_symbol, lv_color_hex(0x111111), 0);
    }
    lv_obj_add_event_cb(play, player_home_play_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *next = player_home_create_round_button(g_overlay, 60, LV_SYMBOL_NEXT);
    lv_obj_align(next, LV_ALIGN_CENTER, 88, 70);
    lv_obj_add_event_cb(next, player_home_next_cb, LV_EVENT_CLICKED, nullptr);

    // 底部音量：拖动只本地预览，松手后提交一次 AudioTask 音量命令；点击百分比切换静音。
    lv_obj_t *volume_status = player_home_create_pill_button(g_overlay, 74, 32, "80%", &g_volume_label);
    lv_obj_align(volume_status, LV_ALIGN_BOTTOM_MID, -100, -52);
    lv_obj_add_event_cb(volume_status, player_home_mute_cb, LV_EVENT_CLICKED, nullptr);

    g_volume_slider = lv_slider_create(g_overlay);
    ui_common_lock_object(g_volume_slider);
    lv_obj_add_flag(g_volume_slider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g_volume_slider, 190, 9);
    lv_obj_align(g_volume_slider, LV_ALIGN_BOTTOM_MID, 34, -63);
    lv_slider_set_range(g_volume_slider, 0, 100);
    lv_slider_set_value(g_volume_slider, 80, LV_ANIM_OFF);
    lv_obj_set_style_radius(g_volume_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_volume_slider, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_volume_slider, 55, LV_PART_MAIN);
    lv_obj_set_style_radius(g_volume_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_volume_slider, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_volume_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_width(g_volume_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_height(g_volume_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_radius(g_volume_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(g_volume_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_volume_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_add_event_cb(g_volume_slider, player_home_volume_cb, LV_EVENT_ALL, nullptr);

    AudioStateSnapshot snapshot = {};
    audio_service_get_snapshot(&snapshot);
    player_home_apply_audio_snapshot(snapshot);
    g_last_audio_state_revision = snapshot.state_revision;

    lv_timer_create(player_home_audio_timer_cb, 100, nullptr);
    lv_timer_create(player_home_artwork_timer_cb, 100, nullptr);
    g_overlay_timer = lv_timer_create(player_home_overlay_timeout_cb, kOverlayTimeoutMs, nullptr);
    if (g_overlay_timer != nullptr) {
        lv_timer_pause(g_overlay_timer);
    }

    // 开机/进入播放器先保持纯封面，不把控制层闪现出来。
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);

    char list_label[96] = {};
    if (!player_state_copy_list_label(list_label, sizeof(list_label))) {
        snprintf(list_label, sizeof(list_label), "未知列表");
    }
    ESP_LOGI(TAG,
        "UI Reset P1.2.5 已启用：全屏 RGB565 单 Surface + Overlay alpha + 5s 自动隐藏；列表=%s 位置=%u/%u track=%u loop=%s volume=%u%% mute=%u",
        list_label,
        static_cast<unsigned>(player_state_get_list_count() > 0 ? player_state_get_list_position() + 1 : 0),
        static_cast<unsigned>(player_state_get_list_count()),
        static_cast<unsigned>(media_library_get_count() > 0 ? player_state_get_index() : 0),
        player_transport_loop_mode_name(player_control_get_loop_mode()),
        static_cast<unsigned>(snapshot.volume_percent),
        static_cast<unsigned>(snapshot.user_muted));
}
