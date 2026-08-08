#include "player_home.h"

#include <stdio.h>
#include "esp_log.h"
#include "audio_service.h"
#include "font/font_manager.h"
#include "media_library.h"
#include "library_view.h"
#include "player_control.h"
#include "player_state.h"
#include "ui_common.h"

static const char *TAG = "首页";
static lv_obj_t *g_title = nullptr;
static lv_obj_t *g_track_info = nullptr;
static lv_obj_t *g_play_symbol = nullptr;
static lv_obj_t *g_progress = nullptr;

// Stage 11.2：进度条使用 0~10000 的归一化范围，避免把超长音频毫秒数直接塞进 LVGL int32_t range。
// 拖动期间只做 UI 本地预览；松手时才向 Player 提交一次 Seek。
static constexpr int32_t kProgressScale = 10000;
static bool g_progress_dragging = false;
static bool g_progress_seek_pending = false;
static uint64_t g_progress_total_ms = 0;
static uint64_t g_progress_preview_ms = 0;
static uint32_t g_progress_track_index = UINT32_MAX;
static uint32_t g_progress_playback_revision = 0;
static uint32_t g_progress_seek_base_revision = 0;
static uint32_t g_progress_seek_started_tick = 0;
static lv_obj_t *g_loop_label = nullptr;
static lv_obj_t *g_volume_label = nullptr;
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
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x252B36), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);

    lv_obj_t *label = player_home_create_label(
        button, symbol, lv_color_hex(0xFFFFFF), lv_font_default());
    lv_obj_center(label);
    return button;
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
        // AudioTask 会把“正好等于总时长”的目标夹到最后一个有效采样点；UI 也提前保持同一语义。
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

static void player_home_show_seek_preview(bool pending)
{
    if (g_track_info == nullptr || g_progress_total_ms == 0U) {
        return;
    }
    char target[24] = {};
    char total[24] = {};
    player_home_format_time(g_progress_preview_ms, target, sizeof(target));
    player_home_format_time(g_progress_total_ms, total, sizeof(total));
    lv_label_set_text_fmt(
        g_track_info,
        pending ? "跳转中  %s / %s" : "预览  %s / %s  · 松手跳转",
        target,
        total);
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

    // 拖动中如果歌曲/播放世代被实体按键或曲库切换，立即取消旧 UI 手势，不能把旧 Track 的目标提交给新歌。
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
        // 异步请求正常应在很短时间内进入 Seeking/完成；10 秒只是防止极端异常时 UI 永久锁在预览值。
        const bool seek_wait_expired = snapshot.state != AudioPlaybackState::Seeking &&
            lv_tick_elaps(g_progress_seek_started_tick) > 10000U;
        if (request_context_stale || seek_committed || seek_failed || seek_wait_expired) {
            g_progress_seek_pending = false;
        }
    }

    player_home_progress_set_enabled(player_home_snapshot_can_scrub(snapshot));

    if (g_progress_dragging || g_progress_seek_pending) {
        player_home_show_seek_preview(g_progress_seek_pending);
        return;
    }

    if (!same_track || total_ms == 0U) {
        lv_slider_set_value(g_progress, 0, LV_ANIM_OFF);
        return;
    }

    lv_slider_set_value(
        g_progress,
        player_home_progress_value_from_ms(snapshot.position_ms, total_ms),
        LV_ANIM_OFF);
}

static void player_home_progress_cb(lv_event_t *event)
{
    if (event == nullptr || g_progress == nullptr) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
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
        player_home_show_seek_preview(false);
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED && g_progress_dragging) {
        g_progress_preview_ms = player_home_progress_ms_from_value(
            lv_slider_get_value(g_progress), g_progress_total_ms);
        player_home_show_seek_preview(false);
        return;
    }

    if (code == LV_EVENT_PRESS_LOST && g_progress_dragging) {
        player_home_cancel_progress_interaction();
        player_home_refresh();
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
        return;
    }

    g_progress_preview_ms = player_home_progress_ms_from_value(
        lv_slider_get_value(g_progress), g_progress_total_ms);
    g_progress_dragging = false;
    g_progress_seek_base_revision = snapshot.seek_revision;
    g_progress_seek_started_tick = lv_tick_get();

    // Stage 11.2：只在松手时提交一次。连续手势不会生成每像素 Seek；后端仍由 11.0.1 intent coalescing 兜底。
    if (player_control_seek_ms(g_progress_preview_ms)) {
        g_progress_seek_pending = true;
        player_home_show_seek_preview(true);
    } else {
        player_home_cancel_progress_interaction();
        player_home_refresh();
    }
}

static lv_obj_t *player_home_create_top_button(
    lv_obj_t *parent,
    int32_t x,
    int32_t width,
    const char *text,
    lv_obj_t **out_label)
{
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_set_pos(button, x, 12);
    lv_obj_set_size(button, width, 36);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1B2029), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);

    lv_obj_t *label = player_home_create_label(
        button, text, lv_color_hex(0xE9ECF1), font_manager_get_ui_font());
    lv_obj_center(label);
    if (out_label != nullptr) {
        *out_label = label;
    }
    return button;
}

static void player_home_refresh_track(const AudioStateSnapshot *audio_snapshot)
{
    if (g_title == nullptr || g_track_info == nullptr) {
        return;
    }

    const size_t library_count = media_library_get_count();
    const size_t list_count = player_state_get_list_count();
    if (library_count == 0 || list_count == 0) {
        lv_label_set_text(g_title, "暂无歌曲");
        lv_label_set_text(g_track_info, "音乐库为空");
        return;
    }

    const size_t index = player_state_get_index();
    const size_t list_position = player_state_get_list_position();
    char title[512] = {};
    if (!media_library_copy_display_name(index, title, sizeof(title))) {
        snprintf(title, sizeof(title), "歌曲 %u", static_cast<unsigned>(index + 1));
    }
    lv_label_set_text(g_title, title);

    char suffix[96] = {};
    if (audio_snapshot != nullptr && audio_snapshot->track_index == index) {
        if (
            audio_snapshot->state == AudioPlaybackState::Error &&
            audio_snapshot->last_error == ESP_ERR_NOT_SUPPORTED
        ) {
            const MediaFormat format = player_state_get_format();
            if (format == MediaFormat::WAV || format == MediaFormat::FLAC || format == MediaFormat::MP3) {
                snprintf(suffix, sizeof(suffix), "  · 参数暂不支持");
            } else {
                snprintf(suffix, sizeof(suffix), "  · 解码器待接入");
            }
        } else if (audio_snapshot->state == AudioPlaybackState::Playing) {
            if (audio_snapshot->sample_rate_hz == 44100) {
                snprintf(suffix, sizeof(suffix), "  · 播放中 · 44.1k/%ubit",
                    static_cast<unsigned>(audio_snapshot->bits_per_sample));
            } else if (audio_snapshot->sample_rate_hz > 0) {
                snprintf(suffix, sizeof(suffix), "  · 播放中 · %luk/%ubit",
                    static_cast<unsigned long>(audio_snapshot->sample_rate_hz / 1000),
                    static_cast<unsigned>(audio_snapshot->bits_per_sample));
            } else {
                snprintf(suffix, sizeof(suffix), "  · 播放中");
            }
        } else if (audio_snapshot->state == AudioPlaybackState::Paused) {
            if (audio_snapshot->sample_rate_hz == 44100) {
                snprintf(suffix, sizeof(suffix), "  · 已暂停 · 44.1k/%ubit",
                    static_cast<unsigned>(audio_snapshot->bits_per_sample));
            } else if (audio_snapshot->sample_rate_hz > 0) {
                snprintf(suffix, sizeof(suffix), "  · 已暂停 · %luk/%ubit",
                    static_cast<unsigned long>(audio_snapshot->sample_rate_hz / 1000),
                    static_cast<unsigned>(audio_snapshot->bits_per_sample));
            } else {
                snprintf(suffix, sizeof(suffix), "  · 已暂停");
            }
        } else if (audio_snapshot->state == AudioPlaybackState::Finished) {
            snprintf(suffix, sizeof(suffix), "  · 播放结束");
        }
    }

    lv_label_set_text_fmt(
        g_track_info,
        "第 %u / %u 首  %s%s",
        static_cast<unsigned>(list_position + 1),
        static_cast<unsigned>(list_count),
        media_format_name(player_state_get_format()),
        suffix);
}

static void player_home_refresh_transport_controls(const AudioStateSnapshot *snapshot)
{
    if (g_loop_label != nullptr) {
        lv_label_set_text(g_loop_label,
            player_transport_loop_mode_name(player_control_get_loop_mode()));
    }
    if (g_volume_label != nullptr && snapshot != nullptr) {
        if (snapshot->user_muted) {
            lv_label_set_text(g_volume_label, "静音");
        } else {
            lv_label_set_text_fmt(g_volume_label, "%u", static_cast<unsigned>(snapshot->volume_percent));
        }
    }
}

static void player_home_apply_audio_snapshot(const AudioStateSnapshot &snapshot)
{
    if (g_play_symbol != nullptr) {
        const bool show_pause = snapshot.state == AudioPlaybackState::Playing;
        lv_label_set_text(g_play_symbol, show_pause ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    player_home_progress_sync(snapshot);

    // 拖动/异步 Seek 等待期间 track_info 用作目标时间预览，避免 100ms snapshot 刷新把提示覆盖掉。
    if (!g_progress_dragging && !g_progress_seek_pending) {
        player_home_refresh_track(&snapshot);
    }
    player_home_refresh_transport_controls(&snapshot);
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
    if (!player_control_toggle_play_pause()) {
        ESP_LOGW(TAG, "播放控制请求未能入队");
    }
}

static void player_home_loop_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    player_control_cycle_loop_mode();
    AudioStateSnapshot snapshot = {};
    audio_service_get_snapshot(&snapshot);
    player_home_refresh_transport_controls(&snapshot);
}

static void player_home_volume_down_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && !player_control_volume_down()) {
        ESP_LOGW(TAG, "降低音量请求未能入队");
    }
}

static void player_home_volume_up_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && !player_control_volume_up()) {
        ESP_LOGW(TAG, "提高音量请求未能入队");
    }
}

static void player_home_mute_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && !player_control_toggle_mute()) {
        ESP_LOGW(TAG, "静音切换请求未能入队");
    }
}

static void player_home_library_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    library_view_open();
}

void player_home_refresh()
{
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot)) {
        return;
    }
    player_home_apply_audio_snapshot(snapshot);
    g_last_audio_state_revision = snapshot.state_revision;
}

void player_home_create(lv_obj_t *screen)
{
    if (screen == nullptr) {
        return;
    }

    player_home_cancel_progress_interaction();
    ui_common_lock_object(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0E1117), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // 顶部控制条：循环模式 / 曲库 / 音量。音量数字本身可点击静音。
    lv_obj_t *loop = player_home_create_top_button(screen, 12, 104, "顺序", &g_loop_label);
    lv_obj_add_event_cb(loop, player_home_loop_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *library = player_home_create_top_button(screen, 192, 76, "曲库", nullptr);
    lv_obj_add_event_cb(library, player_home_library_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *volume_down = player_home_create_top_button(screen, 302, 38, "-", nullptr);
    lv_obj_add_event_cb(volume_down, player_home_volume_down_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *volume = player_home_create_top_button(screen, 344, 58, "80", &g_volume_label);
    lv_obj_add_event_cb(volume, player_home_mute_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *volume_up = player_home_create_top_button(screen, 406, 38, "+", nullptr);
    lv_obj_add_event_cb(volume_up, player_home_volume_up_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *cover = lv_obj_create(screen);
    ui_common_lock_object(cover);
    lv_obj_set_size(cover, 220, 220);
    lv_obj_align(cover, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_style_radius(cover, 26, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x1B2029), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_shadow_width(cover, 0, 0);

    lv_obj_t *cover_icon = player_home_create_label(
        cover, LV_SYMBOL_AUDIO, lv_color_hex(0x6F7A89), lv_font_default());
    lv_obj_center(cover_icon);

    g_title = player_home_create_label(
        screen, "", lv_color_hex(0xFFFFFF), font_manager_get_ui_font());
    lv_label_set_long_mode(g_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_title, 400, 32);
    lv_obj_set_style_text_align(g_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 288);

    g_track_info = player_home_create_label(
        screen, "", lv_color_hex(0x7E8795), font_manager_get_ui_font());
    lv_label_set_long_mode(g_track_info, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_track_info, 420, 32);
    lv_obj_set_style_text_align(g_track_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_track_info, LV_ALIGN_TOP_MID, 0, 320);

    // Stage 11.2：用 Slider 替换只读 Bar。视觉仍保持细轨道，但增加圆形 knob，提供可拖动反馈。
    g_progress = lv_slider_create(screen);
    ui_common_lock_object(g_progress);
    lv_obj_set_size(g_progress, 330, 12);
    lv_obj_align(g_progress, LV_ALIGN_TOP_MID, 0, 354);
    lv_slider_set_range(g_progress, 0, kProgressScale);
    lv_slider_set_value(g_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(g_progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_progress, lv_color_hex(0x282E38), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_progress, lv_color_hex(0xF2F3F5), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_width(g_progress, 18, LV_PART_KNOB);
    lv_obj_set_style_height(g_progress, 18, LV_PART_KNOB);
    lv_obj_set_style_radius(g_progress, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(g_progress, lv_color_hex(0xF2F3F5), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_40, LV_PART_INDICATOR | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_40, LV_PART_KNOB | LV_STATE_DISABLED);
    lv_obj_add_event_cb(g_progress, player_home_progress_cb, LV_EVENT_ALL, nullptr);
    player_home_progress_set_enabled(false);

    lv_obj_t *prev = player_home_create_round_button(screen, 58, LV_SYMBOL_PREV);
    lv_obj_align(prev, LV_ALIGN_BOTTOM_MID, -92, -24);
    lv_obj_add_event_cb(prev, player_home_prev_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *play = player_home_create_round_button(screen, 74, LV_SYMBOL_PLAY);
    lv_obj_align(play, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(play, lv_color_hex(0xF2F3F5), 0);
    g_play_symbol = lv_obj_get_child(play, 0);
    if (g_play_symbol != nullptr) {
        lv_obj_set_style_text_color(g_play_symbol, lv_color_hex(0x11151B), 0);
    }
    lv_obj_add_event_cb(play, player_home_play_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *next = player_home_create_round_button(screen, 58, LV_SYMBOL_NEXT);
    lv_obj_align(next, LV_ALIGN_BOTTOM_MID, 92, -24);
    lv_obj_add_event_cb(next, player_home_next_cb, LV_EVENT_CLICKED, nullptr);

    AudioStateSnapshot snapshot = {};
    audio_service_get_snapshot(&snapshot);
    player_home_apply_audio_snapshot(snapshot);
    g_last_audio_state_revision = snapshot.state_revision;
    lv_timer_create(player_home_audio_timer_cb, 100, nullptr);

    char list_label[96] = {};
    if (!player_state_copy_list_label(list_label, sizeof(list_label))) {
        snprintf(list_label, sizeof(list_label), "未知列表");
    }
    ESP_LOGI(TAG, "Stage 11.2 播放器首页已接入可拖动进度：列表=%s 位置=%u/%u 全局track=%u loop=%s volume=%u%% mute=%u",
        list_label,
        static_cast<unsigned>(player_state_get_list_count() > 0 ? player_state_get_list_position() + 1 : 0),
        static_cast<unsigned>(player_state_get_list_count()),
        static_cast<unsigned>(media_library_get_count() > 0 ? player_state_get_index() : 0),
        player_transport_loop_mode_name(player_control_get_loop_mode()),
        static_cast<unsigned>(snapshot.volume_percent),
        static_cast<unsigned>(snapshot.user_muted));
}
