#include "visual_music_app.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_launcher_overlay.h"
#include "app_manager.h"
#include "audio/audio_service.h"
#include "audio/decoders/flac_decoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "font/font_manager.h"
#include "gesture/gesture_router.h"
#include "lvgl.h"
#include "ui_common.h"
#include "visual_music_browser_model.h"
#include "visual_music_midi.h"
#include "visual_music_nsf.h"

static const char *TAG = "电子音流";

namespace
{

enum class VisualMusicPage : uint8_t
{
    Browser = 0,
    MidiLoading,
    MidiWaterfall,
    NsfLoading,
    NsfReady,
};

enum class BrowserLoadPhase : uint8_t
{
    Idle = 0,
    WaitingForAudioWindow,
    Scanning,
};

enum class PlayerLoopMode : uint8_t
{
    Sequential = 0,
    RepeatOne,
    RepeatAll,
};

static constexpr int32_t kHeaderHeight = 68;
static constexpr int32_t kContentMargin = 12;
static constexpr int32_t kRowHeight = 58;
static constexpr int32_t kRowGap = 8;
static constexpr size_t kVisibleRows = 5U;
static constexpr size_t kGestureStepRows = 4U;
static constexpr uint32_t kBrowserTimerPeriodMs = 20U;
static constexpr uint32_t kFlacSafePercent = 90U;
static constexpr size_t kScanBatchNoFlac = 8U;
static constexpr size_t kScanBatchWithFlac = 1U;
static constexpr uint32_t kWaitLogIntervalMs = 1000U;
// MIDI 播放时后台 Music 已暂停，瀑布只按25fps有界刷新；
// Browser/解析阶段仍通过 FLAC 水位策略给后台 Music 的 SD 读取让路。
static constexpr uint32_t kWaterfallFramePeriodMs = 40U;
static constexpr uint32_t kWaterfallTimeLabelPeriodMs = 250U;
static constexpr uint32_t kWaterfallFutureMs = 4000U;
static constexpr uint32_t kWaterfallPastMs = 600U;
static constexpr int32_t kPlayerTitleHeight = 68;
static constexpr int32_t kPlayerControlHeight = 82;
static constexpr int32_t kPlayerWaterfallHeight = 310;

struct RowUi
{
    lv_obj_t *row = nullptr;
    lv_obj_t *name = nullptr;
    lv_obj_t *kind = nullptr;
};

struct BrowserLoadState
{
    BrowserLoadPhase phase = BrowserLoadPhase::Idle;
    VisualMusicBrowser::DirectoryScanSession scan = {};
    uint32_t last_wait_log_ms = 0U;
};

static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_header_back = nullptr;
static lv_obj_t *g_header_title = nullptr;
static lv_obj_t *g_header_line = nullptr;
static lv_obj_t *g_browser_host = nullptr;
static lv_obj_t *g_browser_status = nullptr;
static lv_obj_t *g_browser_position = nullptr;
static RowUi g_rows[kVisibleRows] = {};
static lv_obj_t *g_player_host = nullptr;
static lv_obj_t *g_player_format = nullptr;
static lv_obj_t *g_player_title = nullptr;
static lv_obj_t *g_player_message = nullptr;
static lv_obj_t *g_player_hint = nullptr;
static lv_obj_t *g_waterfall_widget = nullptr;
static lv_obj_t *g_player_time = nullptr;
static lv_obj_t *g_player_controls = nullptr;
static lv_obj_t *g_loop_button = nullptr;
static lv_obj_t *g_loop_label = nullptr;
static lv_obj_t *g_prev_button = nullptr;
static lv_obj_t *g_play_button = nullptr;
static lv_obj_t *g_play_label = nullptr;
static lv_obj_t *g_next_button = nullptr;
static lv_obj_t *g_list_button = nullptr;
static lv_timer_t *g_timer = nullptr;
static VisualMusicPage g_page = VisualMusicPage::Browser;
static VisualMusicBrowser::DirectorySnapshot g_directory = {};
static BrowserLoadState g_browser_load = {};
static size_t g_first_index = 0U;
static size_t g_selected_index = SIZE_MAX;
static char *g_current_dir = nullptr;
static char *g_scratch_path = nullptr;
static char *g_selected_path = nullptr;
static VisualMusicMidi::Timeline g_midi_timeline = {};
static VisualMusicNsf::Image g_nsf_image = {};
static uint8_t g_nsf_track = 0U;
static uint8_t g_midi_pitch_lane[128] = {};
static uint16_t g_midi_pitch_lane_count = 0U;
static uint32_t g_last_waterfall_draw_tick = 0U;
static uint32_t g_last_time_label_tick = 0U;
static bool g_midi_paused = true;
static bool g_midi_audio_active = false;
static bool g_music_paused_for_midi = false;
static bool g_midi_eof_action_handled = false;
static bool g_nsf_paused = true;
static bool g_nsf_audio_active = false;
static bool g_nsf_eof = false;
static bool g_nsf_eof_action_handled = false;
static bool g_nsf_failed = false;
static bool g_music_paused_for_nsf = false;
static uint32_t g_last_nsf_time_label_tick = 0U;
static PlayerLoopMode g_loop_mode = PlayerLoopMode::Sequential;

static void set_visible(lv_obj_t *obj, bool visible)
{
    if (obj == nullptr) return;
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_label(
    lv_obj_t *parent,
    const char *text,
    uint32_t rgb,
    lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == nullptr) return nullptr;
    ui_common_lock_object(label);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_style_text_font(label, font_manager_get_ui_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static void control_capture_cb(lv_event_t *event)
{
    if (event == nullptr) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) gesture_router_set_control_capture(true);
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        gesture_router_set_control_capture(false);
    }
}

static lv_obj_t *make_control_button(
    lv_obj_t *parent,
    int32_t size,
    const char *text,
    bool symbol_font,
    lv_obj_t **out_label)
{
    lv_obj_t *button = lv_button_create(parent);
    if (button == nullptr) return nullptr;
    ui_common_lock_object(button);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(button, 34, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, control_capture_cb, LV_EVENT_ALL, nullptr);

    lv_obj_t *label = lv_label_create(button);
    if (label == nullptr) {
        lv_obj_delete(button);
        return nullptr;
    }
    ui_common_lock_object(label);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_style_text_font(label, symbol_font ? lv_font_default() : font_manager_get_ui_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xF2F4F7), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    if (out_label != nullptr) *out_label = label;
    return button;
}

static void set_browser_header_visible(bool visible)
{
    set_visible(g_header_back, visible);
    set_visible(g_header_title, visible);
    set_visible(g_header_line, visible);
}

static const char *loop_mode_text()
{
    switch (g_loop_mode) {
        case PlayerLoopMode::RepeatOne: return "单曲";
        case PlayerLoopMode::RepeatAll: return "循环";
        default: return "顺序";
    }
}

static const char *loop_mode_symbol()
{
    switch (g_loop_mode) {
        case PlayerLoopMode::RepeatOne: return LV_SYMBOL_LOOP "1";
        case PlayerLoopMode::RepeatAll: return LV_SYMBOL_LOOP;
        default: return LV_SYMBOL_RIGHT;
    }
}

static void update_player_controls()
{
    if (g_loop_label != nullptr) lv_label_set_text(g_loop_label, loop_mode_symbol());
    const bool midi_playing =
        g_page == VisualMusicPage::MidiWaterfall && g_midi_audio_active && !g_midi_paused;
    const bool nsf_playing =
        g_page == VisualMusicPage::NsfReady && g_nsf_audio_active &&
        !g_nsf_paused && !g_nsf_eof && !g_nsf_failed;
    if (g_play_label != nullptr) {
        lv_label_set_text(g_play_label, midi_playing || nsf_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    if (g_play_button != nullptr) {
        const bool nsf_supported =
            g_page == VisualMusicPage::NsfReady && g_nsf_image.version == 1U &&
            g_nsf_image.track_count > 0U && g_nsf_image.expansion_chips == 0U &&
            (g_nsf_image.pal_ntsc_bits & 0x03U) != 0x01U;
        if (g_page == VisualMusicPage::MidiWaterfall || nsf_supported) {
            lv_obj_remove_state(g_play_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_play_button, LV_STATE_DISABLED);
        }
    }
}

static const char *basename_of(const char *path)
{
    if (path == nullptr || path[0] == '\0') return "电子音流";
    const char *slash = strrchr(path, '/');
    return slash != nullptr && slash[1] != '\0' ? slash + 1 : path;
}

static bool flac_storage_safe(bool *out_competing = nullptr, uint32_t *out_percent = nullptr)
{
    if (out_competing != nullptr) *out_competing = false;
    if (out_percent != nullptr) *out_percent = 100U;
    FlacStorageWindowSnapshot window = {};
    if (!flac_decoder_get_storage_window(&window) || !window.active ||
        !window.storage_competes || window.capacity_bytes == 0U) {
        return true;
    }
    const uint32_t percent = static_cast<uint32_t>(
        (static_cast<uint64_t>(window.buffered_bytes) * 100ULL + window.capacity_bytes / 2ULL) /
        window.capacity_bytes);
    if (out_competing != nullptr) *out_competing = true;
    if (out_percent != nullptr) *out_percent = percent;
    return percent >= kFlacSafePercent;
}

static void update_header()
{
    if (g_header_title == nullptr) return;
    if (g_page != VisualMusicPage::Browser) {
        lv_label_set_text(g_header_title, "电子音流");
    } else if (g_current_dir != nullptr &&
        strcmp(g_current_dir, VisualMusicBrowser::kRootDirectory) != 0) {
        lv_label_set_text(g_header_title, basename_of(g_current_dir));
    } else {
        lv_label_set_text(g_header_title, "电子音流");
    }
}

static bool pause_music_for_midi_exclusive()
{
    if (g_music_paused_for_midi) return true;

    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        ESP_LOGI(TAG, "MIDI Exclusive：AudioTask未就绪，无活动Music需要暂停");
        return true;
    }
    if (snapshot.state != AudioPlaybackState::Playing) {
        ESP_LOGI(TAG, "MIDI Exclusive：Music当前非Playing(state=%u)，保持原状态",
            static_cast<unsigned>(snapshot.state));
        return true;
    }
    if (!audio_service_pause(true)) {
        ESP_LOGE(TAG, "MIDI Exclusive：暂停Music失败");
        return false;
    }
    g_music_paused_for_midi = true;
    ESP_LOGI(TAG, "MIDI Exclusive：Music已暂停；AudioTask切换到MIDI Synth");
    return true;
}

static bool stop_midi_audio(bool restore_music, const char *reason)
{
    // 同 APP 切曲时 Synth 可能已停但 Paused Music 硬件仍保持“待恢复”状态。
    // restore=true 时始终通知 AudioTask 收口 MIDI 临时硬件；即使 Music 进入 APP 前本来就已暂停，
    // 也必须恢复它的 Paused 硬件，之后是否 Resume 再由 g_music_paused_for_midi 决定。
    if (g_midi_audio_active || restore_music) {
        if (!audio_service_midi_stop(restore_music, true)) {
            ESP_LOGE(TAG, "MIDI Synth停止/恢复Music硬件失败：reason=%s",
                reason != nullptr ? reason : "unknown");
            return false;
        }
        g_midi_audio_active = false;
        g_midi_paused = true;
        g_midi_eof_action_handled = false;
    }

    if (restore_music && g_music_paused_for_midi) {
        if (!audio_service_resume(false)) {
            ESP_LOGW(TAG, "MIDI Exclusive恢复Music请求失败：reason=%s；保留暂停标记等待生命周期重试",
                reason != nullptr ? reason : "unknown");
            return false;
        }
        g_music_paused_for_midi = false;
        ESP_LOGI(TAG, "MIDI Exclusive：已请求恢复Music reason=%s",
            reason != nullptr ? reason : "unknown");
    }
    return true;
}

static bool pause_music_for_nsf_exclusive()
{
    if (g_music_paused_for_nsf) return true;

    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        ESP_LOGI(TAG, "NSF Exclusive：AudioTask未就绪，无活动Music需要暂停");
        return true;
    }
    if (snapshot.state != AudioPlaybackState::Playing) {
        ESP_LOGI(TAG, "NSF Exclusive：Music当前非Playing(state=%u)，保持原状态",
            static_cast<unsigned>(snapshot.state));
        return true;
    }
    if (!audio_service_pause(true)) {
        ESP_LOGE(TAG, "NSF Exclusive：暂停Music失败");
        return false;
    }
    g_music_paused_for_nsf = true;
    ESP_LOGI(TAG, "NSF Exclusive：Music已暂停；AudioTask切换到6502/2A03");
    return true;
}

static bool stop_nsf_audio(bool restore_music, const char *reason)
{
    if (g_nsf_audio_active || restore_music) {
        if (!audio_service_nsf_stop(restore_music, true)) {
            ESP_LOGE(TAG, "NSF停止/恢复Music硬件失败：reason=%s",
                reason != nullptr ? reason : "unknown");
            return false;
        }
        g_nsf_audio_active = false;
        g_nsf_paused = true;
        g_nsf_eof = false;
        g_nsf_eof_action_handled = false;
        g_nsf_failed = false;
    }

    if (restore_music && g_music_paused_for_nsf) {
        if (!audio_service_resume(false)) {
            ESP_LOGW(TAG, "NSF Exclusive恢复Music请求失败：reason=%s；保留暂停标记等待生命周期重试",
                reason != nullptr ? reason : "unknown");
            return false;
        }
        g_music_paused_for_nsf = false;
        ESP_LOGI(TAG, "NSF Exclusive：已请求恢复Music reason=%s",
            reason != nullptr ? reason : "unknown");
    }
    return true;
}

static uint32_t midi_position_ms()
{
    if (g_midi_timeline.duration_ms == 0U) return 0U;
    AudioMidiClockSnapshot clock = {};
    if (g_midi_audio_active && audio_service_midi_get_clock(&clock) && clock.active) {
        g_midi_paused = clock.paused || clock.eof;
        const uint64_t position = clock.position_ms;
        return position >= g_midi_timeline.duration_ms
            ? g_midi_timeline.duration_ms
            : static_cast<uint32_t>(position);
    }
    return 0U;
}

static void format_time_pair(char *out, size_t out_size, uint32_t position_ms, uint32_t duration_ms)
{
    if (out == nullptr || out_size == 0U) return;
    const uint32_t position_seconds = position_ms / 1000U;
    const uint32_t duration_seconds = duration_ms / 1000U;
    snprintf(
        out,
        out_size,
        "%lu:%02lu / %lu:%02lu%s",
        static_cast<unsigned long>(position_seconds / 60U),
        static_cast<unsigned long>(position_seconds % 60U),
        static_cast<unsigned long>(duration_seconds / 60U),
        static_cast<unsigned long>(duration_seconds % 60U),
        g_midi_paused && position_ms < duration_ms ? "  已暂停" : "");
}

static void update_midi_time_label()
{
    if (g_player_time == nullptr || g_page != VisualMusicPage::MidiWaterfall) return;
    char text[64] = {};
    format_time_pair(text, sizeof(text), midi_position_ms(), g_midi_timeline.duration_ms);
    lv_label_set_text(g_player_time, text);
}

static size_t midi_first_candidate(uint32_t earliest_start_ms)
{
    size_t left = 0U;
    size_t right = g_midi_timeline.note_count;
    while (left < right) {
        const size_t mid = left + (right - left) / 2U;
        if (g_midi_timeline.notes[mid].start_ms < earliest_start_ms) left = mid + 1U;
        else right = mid;
    }
    return left;
}

static void rebuild_midi_pitch_lanes()
{
    // 只为当前MIDI真正出现过的音高分配列，避免从未使用的半音留下空列。
    memset(g_midi_pitch_lane, 0xFF, sizeof(g_midi_pitch_lane));
    g_midi_pitch_lane_count = 0U;
    if (g_midi_timeline.notes == nullptr || g_midi_timeline.note_count == 0U) return;

    bool used[128] = {};
    for (size_t i = 0U; i < g_midi_timeline.note_count; ++i) {
        const uint8_t note = g_midi_timeline.notes[i].note;
        if (note < 128U) used[note] = true;
    }
    for (uint16_t note = 0U; note < 128U; ++note) {
        if (!used[note]) continue;
        g_midi_pitch_lane[note] = static_cast<uint8_t>(g_midi_pitch_lane_count);
        ++g_midi_pitch_lane_count;
    }
}

static uint8_t program_color_adjust(uint8_t component, int32_t delta)
{
    const int32_t value = static_cast<int32_t>(component) + delta;
    if (value < 0) return 0U;
    if (value > 255) return 255U;
    return static_cast<uint8_t>(value);
}

static uint32_t midi_program_color(uint8_t program, uint8_t channel)
{
    // GM 128个Program按16个乐器家族选基色，再用家族内Program调整明度；
    // 同一Program颜色固定，同一Channel中途换Program后后续音柱会立即换色。
    static constexpr uint32_t kFamilyColors[16] = {
        0x72B7FF, // Piano
        0x8ED1C5, // Chromatic Percussion
        0xF0C66E, // Organ
        0xE59A6F, // Guitar
        0x78C98D, // Bass
        0xA98BFF, // Strings
        0xF08DB7, // Ensemble
        0xD98AE4, // Brass
        0x63D9D0, // Reed
        0x6E9FFF, // Pipe
        0xC39AE8, // Synth Lead
        0x7FC7FF, // Synth Pad
        0xE5B36F, // Synth Effects
        0x94D66D, // Ethnic
        0xE38B8B, // Percussive
        0xA9B3C1, // Sound Effects
    };
    static constexpr int8_t kVariantDelta[8] = {-18, -10, -4, 4, 10, 16, 22, 28};

    if (channel == 9U) {
        // GM Channel 10为鼓组；Program通常表示鼓组选择，使用独立暖色系但仍随Program变化。
        const int32_t delta = kVariantDelta[program & 0x07U];
        const uint32_t base = 0xF19B68;
        return
            (static_cast<uint32_t>(program_color_adjust((base >> 16U) & 0xFFU, delta)) << 16U) |
            (static_cast<uint32_t>(program_color_adjust((base >> 8U) & 0xFFU, delta)) << 8U) |
            static_cast<uint32_t>(program_color_adjust(base & 0xFFU, delta));
    }

    const uint32_t base = kFamilyColors[(program >> 3U) & 0x0FU];
    const int32_t delta = kVariantDelta[program & 0x07U];
    return
        (static_cast<uint32_t>(program_color_adjust((base >> 16U) & 0xFFU, delta)) << 16U) |
        (static_cast<uint32_t>(program_color_adjust((base >> 8U) & 0xFFU, delta)) << 8U) |
        static_cast<uint32_t>(program_color_adjust(base & 0xFFU, delta));
}

static uint32_t midi_velocity_color(uint32_t color, uint8_t velocity)
{
    // 原方案用半透明矩形表达力度，LVGL每个音柱都要做RGB565 alpha blend。
    // 改为直接按Velocity调暗/调亮Program颜色并全不透明绘制，视觉语义不变但热路径更轻。
    const uint32_t scale = 58U + (static_cast<uint32_t>(velocity) * 42U) / 127U;
    const uint32_t r = (((color >> 16U) & 0xFFU) * scale) / 100U;
    const uint32_t g = (((color >> 8U) & 0xFFU) * scale) / 100U;
    const uint32_t bl = ((color & 0xFFU) * scale) / 100U;
    return (r << 16U) | (g << 8U) | bl;
}

static void waterfall_draw_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN ||
        g_page != VisualMusicPage::MidiWaterfall || g_midi_timeline.notes == nullptr) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    if (layer == nullptr || obj == nullptr) return;

    lv_area_t coords = {};
    lv_obj_get_coords(obj, &coords);
    const int32_t width = coords.x2 - coords.x1 + 1;
    const int32_t height = coords.y2 - coords.y1 + 1;
    if (width <= 8 || height <= 20) return;

    const int32_t top = coords.y1 + 4;
    const int32_t current_line_y = coords.y2 - 28;
    const int32_t future_pixels = current_line_y - top;
    if (future_pixels <= 0) return;

    const uint32_t now_ms = midi_position_ms();
    const uint32_t window_start = now_ms > kWaterfallPastMs ? now_ms - kWaterfallPastMs : 0U;
    const uint32_t window_end =
        UINT32_MAX - now_ms < kWaterfallFutureMs ? UINT32_MAX : now_ms + kWaterfallFutureMs;
    const uint32_t earliest_start = window_start > g_midi_timeline.max_note_duration_ms
        ? window_start - g_midi_timeline.max_note_duration_ms
        : 0U;

    const int32_t pitch_count = static_cast<int32_t>(g_midi_pitch_lane_count);
    if (pitch_count <= 0) return;
    const int32_t lane_width = width / pitch_count;
    int32_t note_width = lane_width > 2 ? lane_width - 2 : 3;
    if (note_width < 3) note_width = 3;
    if (note_width > 20) note_width = 20;

    lv_draw_rect_dsc_t note_dsc = {};
    lv_draw_rect_dsc_init(&note_dsc);
    // 纯瀑布音柱使用直角全不透明矩形，避免圆角/透明混合放大持续刷新成本。
    note_dsc.radius = 0;
    note_dsc.border_width = 0;
    note_dsc.bg_opa = LV_OPA_COVER;

    const size_t first = midi_first_candidate(earliest_start);
    for (size_t i = first; i < g_midi_timeline.note_count; ++i) {
        const VisualMusicMidi::NoteEvent &note = g_midi_timeline.notes[i];
        if (note.start_ms > window_end) break;
        if (note.end_ms < window_start || note.note >= 128U) continue;
        const uint8_t pitch_lane = g_midi_pitch_lane[note.note];
        if (pitch_lane == 0xFFU) continue;

        const int64_t start_delta = static_cast<int64_t>(note.start_ms) - now_ms;
        const int64_t end_delta = static_cast<int64_t>(note.end_ms) - now_ms;
        int32_t y_start = current_line_y - static_cast<int32_t>(
            (start_delta * future_pixels) / static_cast<int64_t>(kWaterfallFutureMs));
        int32_t y_end = current_line_y - static_cast<int32_t>(
            (end_delta * future_pixels) / static_cast<int64_t>(kWaterfallFutureMs));
        int32_t y1 = y_end < y_start ? y_end : y_start;
        int32_t y2 = y_end > y_start ? y_end : y_start;
        if (y2 - y1 < 2) y2 = y1 + 2;
        if (y2 < coords.y1 || y1 > coords.y2) continue;
        if (y1 < coords.y1) y1 = coords.y1;
        if (y2 > coords.y2) y2 = coords.y2;

        const int32_t pitch_index = static_cast<int32_t>(pitch_lane);
        const int32_t center_x = coords.x1 +
            (pitch_index * width + width / 2) / pitch_count;
        lv_area_t area = {};
        area.x1 = center_x - note_width / 2;
        area.x2 = area.x1 + note_width - 1;
        area.y1 = y1;
        area.y2 = y2;
        if (area.x1 < coords.x1) area.x1 = coords.x1;
        if (area.x2 > coords.x2) area.x2 = coords.x2;

        note_dsc.bg_color = lv_color_hex(midi_velocity_color(
            midi_program_color(note.program, note.channel), note.velocity));
        lv_draw_rect(layer, &note_dsc, &area);
    }

    lv_draw_rect_dsc_t line_dsc = {};
    lv_draw_rect_dsc_init(&line_dsc);
    line_dsc.bg_color = lv_color_hex(0xD7E4F5);
    line_dsc.bg_opa = LV_OPA_70;
    line_dsc.radius = 0;
    line_dsc.border_width = 0;
    lv_area_t line = {coords.x1 + 2, current_line_y, coords.x2 - 2, current_line_y};
    lv_draw_rect(layer, &line_dsc, &line);
}

static void cancel_midi_player()
{
    VisualMusicMidi::cancel();
    VisualMusicMidi::release_timeline(&g_midi_timeline);
    memset(g_midi_pitch_lane, 0xFF, sizeof(g_midi_pitch_lane));
    g_midi_pitch_lane_count = 0U;
    g_last_waterfall_draw_tick = 0U;
    g_last_time_label_tick = 0U;
    g_midi_paused = true;
    g_midi_eof_action_handled = false;
}

static void cancel_nsf_player()
{
    VisualMusicNsf::cancel();
    VisualMusicNsf::release_image(&g_nsf_image);
    g_nsf_track = 0U;
    g_nsf_eof = false;
    g_nsf_eof_action_handled = false;
    g_last_nsf_time_label_tick = 0U;
}

static void set_player_loading_ui(const char *message, const char *hint)
{
    set_visible(g_waterfall_widget, false);
    set_visible(g_player_time, false);
    set_visible(g_player_message, true);
    set_visible(g_player_hint, true);
    if (g_player_message != nullptr) lv_label_set_text(g_player_message, message != nullptr ? message : "");
    if (g_player_hint != nullptr) lv_label_set_text(g_player_hint, hint != nullptr ? hint : "");
    update_player_controls();
}

static void begin_midi_load()
{
    if (g_selected_path == nullptr || g_selected_path[0] == '\0') return;
    // 同一 MIDI 文件切换仍避免恢复 Music；若当前是 NSF，则先完整交还音频硬件。
    (void)stop_midi_audio(false, "switch_midi");
    (void)stop_nsf_audio(true, "switch_nsf_to_midi");
    cancel_midi_player();
    cancel_nsf_player();
    g_page = VisualMusicPage::MidiLoading;
    set_visible(g_browser_host, false);
    set_browser_header_visible(false);
    set_visible(g_player_host, true);
    if (g_player_format != nullptr) lv_label_set_text(g_player_format, "MIDI");
    if (g_player_title != nullptr) lv_label_set_text(g_player_title, basename_of(g_selected_path));
    set_player_loading_ui("正在解析 MIDI…", "右滑或点击“列表”返回文件列表");
    const esp_err_t ret = VisualMusicMidi::start(g_selected_path);
    if (ret != ESP_OK) {
        (void)stop_midi_audio(true, "midi_parser_start_failed");
        set_player_loading_ui("MIDI解析任务启动失败", esp_err_to_name(ret));
        ESP_LOGW(TAG, "MIDI解析任务启动失败：path=%s ret=%s", g_selected_path, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "MIDI解析已排队：%s", g_selected_path);
    }
    gesture_router_reset();
}

static void update_nsf_time_label()
{
    if (g_player_time == nullptr || g_nsf_image.track_count == 0U) return;
    uint64_t position_ms = 0ULL;
    AudioNsfClockSnapshot clock = {};
    if (g_nsf_audio_active && audio_service_nsf_get_clock(&clock) && clock.active) {
        position_ms = clock.position_ms;
        g_nsf_paused = clock.paused || clock.eof;
        g_nsf_eof = clock.eof;
        g_nsf_failed = clock.failed;
    }
    const uint64_t seconds = position_ms / 1000ULL;
    char duration_text[24] = "--:--";
    if (clock.duration_ms > 0ULL) {
        const uint64_t duration_seconds = clock.duration_ms / 1000ULL;
        snprintf(duration_text, sizeof(duration_text), "%llu:%02llu",
            static_cast<unsigned long long>(duration_seconds / 60ULL),
            static_cast<unsigned long long>(duration_seconds % 60ULL));
    }
    char text[48] = {};
    snprintf(
        text,
        sizeof(text),
        "%llu:%02llu / %s",
        static_cast<unsigned long long>(seconds / 60ULL),
        static_cast<unsigned long long>(seconds % 60ULL),
        duration_text);
    lv_label_set_text(g_player_time, text);
}

static void update_nsf_ready_ui()
{
    if (g_page != VisualMusicPage::NsfReady || g_nsf_image.track_count == 0U) return;
    set_visible(g_waterfall_widget, false);
    set_visible(g_player_time, true);
    set_visible(g_player_message, true);
    set_visible(g_player_hint, true);

    if (g_player_title != nullptr) {
        lv_label_set_text(
            g_player_title,
            g_nsf_image.song_name[0] != '\0' ? g_nsf_image.song_name : basename_of(g_selected_path));
    }
    if (g_player_format != nullptr) {
        char format[96] = {};
        snprintf(
            format,
            sizeof(format),
            "Track %u/%u",
            static_cast<unsigned>(g_nsf_track + 1U),
            static_cast<unsigned>(g_nsf_image.track_count));
        lv_label_set_text(g_player_format, format);
    }
    update_nsf_time_label();

    if (g_player_message != nullptr) {
        char message[160] = {};
        if (g_nsf_image.version != 1U) {
            snprintf(message, sizeof(message), "NSF v%u 暂未支持\n作者：%s",
                static_cast<unsigned>(g_nsf_image.version),
                g_nsf_image.artist[0] != '\0' ? g_nsf_image.artist : "未知");
        } else if (g_nsf_image.expansion_chips != 0U) {
            snprintf(
                message,
                sizeof(message),
                "扩展音源 0x%02X 暂未支持\n作者：%s",
                static_cast<unsigned>(g_nsf_image.expansion_chips),
                g_nsf_image.artist[0] != '\0' ? g_nsf_image.artist : "未知");
        } else if ((g_nsf_image.pal_ntsc_bits & 0x03U) == 0x01U) {
            snprintf(message, sizeof(message), "纯 PAL NSF 暂未支持\n作者：%s",
                g_nsf_image.artist[0] != '\0' ? g_nsf_image.artist : "未知");
        } else if (g_nsf_failed) {
            snprintf(message, sizeof(message), "NSF播放失败，可点击播放键重试\n作者：%s",
                g_nsf_image.artist[0] != '\0' ? g_nsf_image.artist : "未知");
        } else {
            snprintf(message, sizeof(message), "2A03 基础5通道实时播放\n作者：%s",
                g_nsf_image.artist[0] != '\0' ? g_nsf_image.artist : "未知");
        }
        lv_label_set_text(g_player_message, message);
    }
    if (g_player_hint != nullptr) {
        char hint[180] = {};
        snprintf(
            hint,
            sizeof(hint),
            "Load $%04X · Init $%04X · Play $%04X · PRG %uB\nPulse 1/2 · Triangle · Noise · DPCM · 48k PCM",
            static_cast<unsigned>(g_nsf_image.load_address),
            static_cast<unsigned>(g_nsf_image.init_address),
            static_cast<unsigned>(g_nsf_image.play_address),
            static_cast<unsigned>(g_nsf_image.prg_size));
        lv_label_set_text(g_player_hint, hint);
    }
    update_player_controls();
}

static bool start_nsf_audio_from_image()
{
    if (g_nsf_image.prg_data == nullptr || g_nsf_image.prg_size == 0U ||
        g_nsf_image.track_count == 0U) {
        return false;
    }
    if (g_nsf_image.version != 1U || g_nsf_image.expansion_chips != 0U ||
        (g_nsf_image.pal_ntsc_bits & 0x03U) == 0x01U) {
        update_nsf_ready_ui();
        return false;
    }
    if (!pause_music_for_nsf_exclusive()) {
        set_player_loading_ui("无法暂停后台Music", "返回列表后可继续浏览文件");
        return false;
    }

    NsfSynthConfig config = {};
    config.load_address = g_nsf_image.load_address;
    config.init_address = g_nsf_image.init_address;
    config.play_address = g_nsf_image.play_address;
    config.ntsc_speed_us = g_nsf_image.ntsc_speed_us;
    memcpy(config.banks, g_nsf_image.banks, sizeof(config.banks));
    config.version = g_nsf_image.version;
    config.track_count = g_nsf_image.track_count;
    config.track = g_nsf_track;
    config.pal_ntsc_bits = g_nsf_image.pal_ntsc_bits;
    config.expansion_chips = g_nsf_image.expansion_chips;

    if (!audio_service_nsf_start(
            g_nsf_image.prg_data,
            g_nsf_image.prg_size,
            &config,
            true)) {
        (void)stop_nsf_audio(true, "nsf_start_failed");
        g_nsf_failed = true;
        update_nsf_ready_ui();
        ESP_LOGE(TAG, "NSF 2A03启动失败：track=%u/%u",
            static_cast<unsigned>(g_nsf_track + 1U),
            static_cast<unsigned>(g_nsf_image.track_count));
        return false;
    }

    g_nsf_audio_active = true;
    g_nsf_paused = false;
    g_nsf_eof = false;
    g_nsf_eof_action_handled = false;
    g_nsf_failed = false;
    g_last_nsf_time_label_tick = 0U;
    update_nsf_ready_ui();
    ESP_LOGI(TAG, "NSF 2A03音频启动：track=%u/%u 48000Hz 基础5通道",
        static_cast<unsigned>(g_nsf_track + 1U),
        static_cast<unsigned>(g_nsf_image.track_count));
    return true;
}

static void begin_nsf_load()
{
    if (g_selected_path == nullptr || g_selected_path[0] == '\0') return;
    // 切换到新的 NSF 文件时先完整交还上一种临时音源；解析阶段允许原 Music 继续播放。
    (void)stop_midi_audio(true, "switch_to_nsf");
    (void)stop_nsf_audio(true, "switch_nsf_file");
    cancel_midi_player();
    cancel_nsf_player();
    g_nsf_paused = true;
    g_nsf_eof = false;
    g_nsf_eof_action_handled = false;
    g_nsf_failed = false;
    g_page = VisualMusicPage::NsfLoading;
    set_visible(g_browser_host, false);
    set_browser_header_visible(false);
    set_visible(g_player_host, true);
    if (g_player_format != nullptr) lv_label_set_text(g_player_format, "NSF / NSFE");
    if (g_player_title != nullptr) lv_label_set_text(g_player_title, basename_of(g_selected_path));
    set_player_loading_ui("正在解析 NSF…", "右滑或点击列表返回文件列表");
    const esp_err_t ret = VisualMusicNsf::start(g_selected_path);
    if (ret != ESP_OK) {
        set_player_loading_ui("NSF解析任务启动失败", esp_err_to_name(ret));
        ESP_LOGW(TAG, "NSF解析任务启动失败：path=%s ret=%s", g_selected_path, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "NSF解析已排队：%s", g_selected_path);
    }
    gesture_router_reset();
}

static void nsf_result_tick()
{
    if (g_page != VisualMusicPage::NsfLoading) return;
    VisualMusicNsf::LoadResult result = {};
    if (!VisualMusicNsf::take_result(&result)) return;
    if (result.state != VisualMusicNsf::LoadState::Ready || result.result != ESP_OK) {
        VisualMusicNsf::release_image(&result.image);
        const char *message = result.result == ESP_ERR_NOT_SUPPORTED
            ? "NSFE容器尚未接入"
            : "NSF解析失败";
        set_player_loading_ui(message, esp_err_to_name(result.result));
        ESP_LOGW(TAG, "NSF镜像不可用：ret=%s", esp_err_to_name(result.result));
        return;
    }

    VisualMusicNsf::release_image(&g_nsf_image);
    g_nsf_image = result.image;
    result.image = {};
    g_nsf_track = g_nsf_image.initial_track;
    g_nsf_paused = true;
    g_nsf_eof = false;
    g_nsf_eof_action_handled = false;
    g_nsf_failed = false;
    g_page = VisualMusicPage::NsfReady;
    update_nsf_ready_ui();
    gesture_router_reset();
    ESP_LOGI(
        TAG,
        "NSF Subsong已就绪：track=%u/%u expansion=0x%02X",
        static_cast<unsigned>(g_nsf_track + 1U),
        static_cast<unsigned>(g_nsf_image.track_count),
        static_cast<unsigned>(g_nsf_image.expansion_chips));

    if (g_nsf_image.version == 1U && g_nsf_image.expansion_chips == 0U &&
        (g_nsf_image.pal_ntsc_bits & 0x03U) != 0x01U) {
        (void)start_nsf_audio_from_image();
    }
}

static bool select_nsf_subsong(int direction, bool allow_wrap)
{
    if (g_page != VisualMusicPage::NsfReady || g_nsf_image.track_count == 0U || direction == 0) {
        return false;
    }
    int next = static_cast<int>(g_nsf_track) + (direction > 0 ? 1 : -1);
    if (next < 0) {
        if (!allow_wrap) return false;
        next = static_cast<int>(g_nsf_image.track_count) - 1;
    } else if (next >= static_cast<int>(g_nsf_image.track_count)) {
        if (!allow_wrap) return false;
        next = 0;
    }

    const uint8_t next_track = static_cast<uint8_t>(next);
    const bool restarting_from_eof = g_nsf_eof;
    if (g_nsf_audio_active && !audio_service_nsf_set_track(next_track, true)) {
        ESP_LOGE(TAG, "NSF Subsong切换失败：track=%u/%u",
            static_cast<unsigned>(next_track + 1U),
            static_cast<unsigned>(g_nsf_image.track_count));
        return false;
    }
    g_nsf_track = next_track;
    if (restarting_from_eof) g_nsf_paused = false;
    g_nsf_eof = false;
    g_nsf_eof_action_handled = false;
    g_nsf_failed = false;
    g_last_nsf_time_label_tick = 0U;
    update_nsf_ready_ui();
    ESP_LOGI(
        TAG,
        "NSF Subsong选择：track=%u/%u",
        static_cast<unsigned>(g_nsf_track + 1U),
        static_cast<unsigned>(g_nsf_image.track_count));
    return true;
}

static void midi_result_tick()
{
    if (g_page != VisualMusicPage::MidiLoading) return;
    VisualMusicMidi::LoadResult result = {};
    if (!VisualMusicMidi::take_result(&result)) return;
    if (result.state != VisualMusicMidi::LoadState::Ready || result.result != ESP_OK) {
        VisualMusicMidi::release_timeline(&result.timeline);
        (void)stop_midi_audio(true, "midi_parse_failed");
        set_player_loading_ui("MIDI解析失败", esp_err_to_name(result.result));
        ESP_LOGW(TAG, "MIDI时间轴不可用：ret=%s", esp_err_to_name(result.result));
        return;
    }

    VisualMusicMidi::release_timeline(&g_midi_timeline);
    g_midi_timeline = result.timeline;
    result.timeline = {};
    rebuild_midi_pitch_lanes();

    if (!pause_music_for_midi_exclusive()) {
        set_player_loading_ui("无法暂停后台Music", "返回列表后可继续浏览文件");
        return;
    }
    if (!audio_service_midi_start(
            g_midi_timeline.notes,
            g_midi_timeline.note_count,
            g_midi_timeline.duration_ms,
            true)) {
        (void)stop_midi_audio(true, "midi_start_failed");
        set_player_loading_ui("MIDI音频启动失败", "AudioTask未能接管MIDI Synth");
        ESP_LOGE(TAG, "MIDI Synth启动失败：notes=%u duration=%lums",
            static_cast<unsigned>(g_midi_timeline.note_count),
            static_cast<unsigned long>(g_midi_timeline.duration_ms));
        return;
    }

    g_midi_audio_active = true;
    g_last_waterfall_draw_tick = 0U;
    g_last_time_label_tick = 0U;
    g_midi_paused = false;
    g_midi_eof_action_handled = false;
    g_page = VisualMusicPage::MidiWaterfall;

    char format[96] = {};
    snprintf(
        format,
        sizeof(format),
        "MIDI瀑布 · %u音符 · %u轨",
        static_cast<unsigned>(g_midi_timeline.note_count),
        static_cast<unsigned>(g_midi_timeline.track_count));
    if (g_player_format != nullptr) lv_label_set_text(g_player_format, format);
    set_visible(g_player_message, false);
    set_visible(g_player_hint, false);
    set_visible(g_waterfall_widget, true);
    set_visible(g_player_time, true);
    update_midi_time_label();
    update_player_controls();
    if (g_waterfall_widget != nullptr) lv_obj_invalidate(g_waterfall_widget);
    ESP_LOGI(
        TAG,
        "MIDI瀑布+音频启动：notes=%u lanes=%u range=%u-%u duration=%lums",
        static_cast<unsigned>(g_midi_timeline.note_count),
        static_cast<unsigned>(g_midi_pitch_lane_count),
        static_cast<unsigned>(g_midi_timeline.min_note),
        static_cast<unsigned>(g_midi_timeline.max_note),
        static_cast<unsigned long>(g_midi_timeline.duration_ms));
}

static void show_browser();

static void toggle_midi_playback()
{
    if (g_page != VisualMusicPage::MidiWaterfall || !g_midi_audio_active) return;
    AudioMidiClockSnapshot clock = {};
    if (!audio_service_midi_get_clock(&clock) || !clock.active) return;

    bool success = false;
    if (clock.eof) {
        success = audio_service_midi_restart(true);
        if (success) g_midi_paused = false;
    } else if (clock.paused) {
        success = audio_service_midi_resume(true);
        if (success) g_midi_paused = false;
    } else {
        success = audio_service_midi_pause(true);
        if (success) g_midi_paused = true;
    }
    if (!success) return;
    g_midi_eof_action_handled = false;

    update_midi_time_label();
    update_player_controls();
    if (g_waterfall_widget != nullptr) lv_obj_invalidate(g_waterfall_widget);
}

static void toggle_nsf_playback()
{
    if (g_page != VisualMusicPage::NsfReady) return;
    if (!g_nsf_audio_active) {
        (void)start_nsf_audio_from_image();
        return;
    }

    AudioNsfClockSnapshot clock = {};
    if (!audio_service_nsf_get_clock(&clock) || !clock.active || clock.failed) return;
    bool success = false;
    if (clock.eof) {
        success = audio_service_nsf_set_track(g_nsf_track, true);
        if (success) {
            g_nsf_paused = false;
            g_nsf_eof = false;
            g_nsf_eof_action_handled = false;
        }
    } else if (clock.paused) {
        success = audio_service_nsf_resume(true);
        if (success) g_nsf_paused = false;
    } else {
        success = audio_service_nsf_pause(true);
        if (success) g_nsf_paused = true;
    }
    if (!success) return;
    update_nsf_time_label();
    update_player_controls();
}

static bool select_playable_index(size_t index)
{
    const VisualMusicBrowser::EntryIndex *entry = VisualMusicBrowser::entry_at(&g_directory, index);
    const char *name = VisualMusicBrowser::entry_name(&g_directory, index);
    if (entry == nullptr || name == nullptr || VisualMusicBrowser::entry_is_directory(entry) ||
        (!VisualMusicBrowser::entry_is_midi(entry) && !VisualMusicBrowser::entry_is_nsf(entry)) ||
        VisualMusicBrowser::join_child_path(
            g_current_dir,
            name,
            g_selected_path,
            VisualMusicBrowser::kPathBytes) != ESP_OK) {
        return false;
    }

    g_selected_index = index;
    if (VisualMusicBrowser::entry_is_midi(entry)) begin_midi_load();
    else begin_nsf_load();
    return true;
}

static bool select_adjacent_track(int direction, bool allow_wrap)
{
    if (g_directory.count == 0U || direction == 0) return false;
    size_t index = g_selected_index < g_directory.count
        ? g_selected_index
        : (direction > 0 ? SIZE_MAX : 0U);

    for (size_t attempt = 0U; attempt < g_directory.count; ++attempt) {
        if (direction > 0) {
            if (index == SIZE_MAX) index = 0U;
            else if (index + 1U < g_directory.count) ++index;
            else if (allow_wrap) index = 0U;
            else return false;
        } else {
            if (index == 0U) {
                if (allow_wrap) index = g_directory.count - 1U;
                else return false;
            } else {
                --index;
            }
        }
        if (select_playable_index(index)) return true;
    }
    return false;
}

static void loop_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click()) return;
    switch (g_loop_mode) {
        case PlayerLoopMode::Sequential: g_loop_mode = PlayerLoopMode::RepeatOne; break;
        case PlayerLoopMode::RepeatOne: g_loop_mode = PlayerLoopMode::RepeatAll; break;
        default: g_loop_mode = PlayerLoopMode::Sequential; break;
    }
    update_player_controls();
    ESP_LOGI(TAG, "电子音流循环模式：%s", loop_mode_text());
}

static void prev_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click()) return;
    if (g_page == VisualMusicPage::NsfReady) {
        (void)select_nsf_subsong(-1, g_loop_mode == PlayerLoopMode::RepeatAll);
        return;
    }
    select_adjacent_track(-1, g_loop_mode == PlayerLoopMode::RepeatAll);
}

static void play_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click()) return;
    if (g_page == VisualMusicPage::MidiWaterfall) toggle_midi_playback();
    else if (g_page == VisualMusicPage::NsfReady) toggle_nsf_playback();
}

static void next_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click()) return;
    if (g_page == VisualMusicPage::NsfReady) {
        (void)select_nsf_subsong(+1, g_loop_mode == PlayerLoopMode::RepeatAll);
        return;
    }
    select_adjacent_track(+1, g_loop_mode == PlayerLoopMode::RepeatAll);
}

static void list_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click()) return;
    show_browser();
}

static void show_status(const char *text, uint32_t color)
{
    if (g_browser_host == nullptr) return;
    for (RowUi &ui : g_rows) set_visible(ui.row, false);
    set_visible(g_browser_position, false);
    if (g_browser_status == nullptr) {
        g_browser_status = make_label(g_browser_host, text, color, LV_TEXT_ALIGN_CENTER);
        if (g_browser_status != nullptr) {
            lv_obj_set_width(g_browser_status, 390);
            lv_label_set_long_mode(g_browser_status, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(g_browser_status, 8, 0);
            lv_obj_align(g_browser_status, LV_ALIGN_TOP_MID, 0, 110);
        }
    } else {
        lv_label_set_text(g_browser_status, text != nullptr ? text : "");
        lv_obj_set_style_text_color(g_browser_status, lv_color_hex(color), 0);
        set_visible(g_browser_status, true);
    }
}

static void update_rows()
{
    if (g_directory.entries == nullptr || g_directory.count == 0U) {
        show_status("/synth 中没有 MIDI / NSF", 0x7F8A99);
        return;
    }
    if (g_browser_status != nullptr) set_visible(g_browser_status, false);

    const size_t max_first = g_directory.count > kVisibleRows
        ? g_directory.count - kVisibleRows
        : 0U;
    if (g_first_index > max_first) g_first_index = max_first;

    size_t last = g_first_index;
    for (size_t slot = 0U; slot < kVisibleRows; ++slot) {
        RowUi &ui = g_rows[slot];
        const size_t index = g_first_index + slot;
        if (ui.row == nullptr || ui.name == nullptr || ui.kind == nullptr ||
            index >= g_directory.count) {
            if (ui.row != nullptr) set_visible(ui.row, false);
            continue;
        }

        const VisualMusicBrowser::EntryIndex *entry =
            VisualMusicBrowser::entry_at(&g_directory, index);
        const char *name = VisualMusicBrowser::entry_name(&g_directory, index);
        if (entry == nullptr || name == nullptr) {
            set_visible(ui.row, false);
            continue;
        }

        const bool is_dir = VisualMusicBrowser::entry_is_directory(entry);
        const bool is_midi = VisualMusicBrowser::entry_is_midi(entry);
        lv_label_set_text(ui.name, name);
        lv_label_set_text(ui.kind, VisualMusicBrowser::entry_kind_name(entry));
        lv_obj_set_style_text_color(
            ui.name,
            lv_color_hex(is_dir ? 0xBCD7FF : 0xEEF1F5),
            0);
        lv_obj_set_style_text_color(
            ui.kind,
            lv_color_hex(is_dir ? 0x6EA5F2 : (is_midi ? 0x77B5FF : 0xC79BFF)),
            0);
        lv_obj_set_style_bg_color(
            ui.row,
            lv_color_hex(is_dir ? 0x172230 : 0x151A21),
            0);
        set_visible(ui.row, true);
        last = index;
    }

    if (g_browser_position != nullptr) {
        char position[64] = {};
        snprintf(
            position,
            sizeof(position),
            "%u-%u / %u",
            static_cast<unsigned>(g_first_index + 1U),
            static_cast<unsigned>(last + 1U),
            static_cast<unsigned>(g_directory.count));
        lv_label_set_text(g_browser_position, position);
        set_visible(g_browser_position, true);
    }
}

static void shift_window(int direction)
{
    if (g_page != VisualMusicPage::Browser ||
        g_browser_load.phase != BrowserLoadPhase::Idle ||
        g_directory.entries == nullptr || g_directory.count <= kVisibleRows || direction == 0) {
        return;
    }
    const size_t old = g_first_index;
    const size_t max_first = g_directory.count - kVisibleRows;
    if (direction > 0) {
        const size_t candidate = old + kGestureStepRows;
        g_first_index = candidate < max_first ? candidate : max_first;
    } else {
        g_first_index = old > kGestureStepRows ? old - kGestureStepRows : 0U;
    }
    if (old == g_first_index) return;
    update_rows();
    ESP_LOGI(
        TAG,
        "电子音流虚拟目录：first=%u total=%u direction=%s",
        static_cast<unsigned>(g_first_index),
        static_cast<unsigned>(g_directory.count),
        direction > 0 ? "NEXT" : "PREV");
}

static void cancel_browser_load()
{
    VisualMusicBrowser::cancel_directory_scan(&g_browser_load.scan);
    g_browser_load = {};
}

static void begin_browser_load()
{
    cancel_browser_load();
    VisualMusicBrowser::release_directory(&g_directory);
    g_first_index = 0U;
    g_browser_load.phase = BrowserLoadPhase::WaitingForAudioWindow;
    show_status("正在加载电子音流目录…", 0x8E9AAA);
    ESP_LOGI(
        TAG,
        "电子音流目录协作加载已排队：%s",
        g_current_dir != nullptr ? g_current_dir : "(null)");
}

static void finish_browser_load()
{
    const esp_err_t ret = VisualMusicBrowser::finish_directory_scan(
        &g_browser_load.scan,
        &g_directory);
    g_browser_load = {};
    if (ret != ESP_OK) {
        show_status("电子音流目录加载失败", 0xE18A8A);
        ESP_LOGW(TAG, "电子音流目录完成失败：%s", esp_err_to_name(ret));
        return;
    }

    update_rows();
    ESP_LOGI(
        TAG,
        "电子音流目录索引：%s items=%u index=%uB strings=%uB virtual_rows=%u",
        g_current_dir,
        static_cast<unsigned>(g_directory.count),
        static_cast<unsigned>(g_directory.count * sizeof(VisualMusicBrowser::EntryIndex)),
        static_cast<unsigned>(g_directory.pool_size),
        static_cast<unsigned>(kVisibleRows));
}

static void browser_load_tick()
{
    if (g_page != VisualMusicPage::Browser ||
        g_browser_load.phase == BrowserLoadPhase::Idle ||
        g_current_dir == nullptr || app_manager_foreground() != AppId::Nsf) {
        return;
    }

    bool competing = false;
    uint32_t percent = 100U;
    if (!flac_storage_safe(&competing, &percent)) {
        const uint32_t now = static_cast<uint32_t>(lv_tick_get());
        if (now - g_browser_load.last_wait_log_ms >= kWaitLogIntervalMs) {
            g_browser_load.last_wait_log_ms = now;
            ESP_LOGI(
                TAG,
                "电子音流目录加载让路FLAC：ring=%u%% < %u%% phase=%u",
                static_cast<unsigned>(percent),
                static_cast<unsigned>(kFlacSafePercent),
                static_cast<unsigned>(g_browser_load.phase));
        }
        return;
    }

    if (g_browser_load.phase == BrowserLoadPhase::WaitingForAudioWindow) {
        const esp_err_t ret = VisualMusicBrowser::begin_directory_scan(
            g_current_dir,
            &g_browser_load.scan);
        if (ret != ESP_OK) {
            g_browser_load = {};
            show_status(
                ret == ESP_ERR_NOT_FOUND
                    ? "请在TF卡根目录创建 synth 文件夹"
                    : "电子音流目录打开失败",
                0xE18A8A);
            ESP_LOGW(
                TAG,
                "电子音流目录打开失败：path=%s ret=%s",
                g_current_dir,
                esp_err_to_name(ret));
            return;
        }
        g_browser_load.phase = BrowserLoadPhase::Scanning;
    }

    bool done = false;
    const size_t batch = competing ? kScanBatchWithFlac : kScanBatchNoFlac;
    const esp_err_t ret = VisualMusicBrowser::scan_directory_step(
        &g_browser_load.scan,
        batch,
        &done);
    if (ret == ESP_ERR_TIMEOUT) return;
    if (ret != ESP_OK) {
        cancel_browser_load();
        show_status("电子音流目录扫描失败", 0xE18A8A);
        ESP_LOGW(TAG, "电子音流目录扫描失败：%s", esp_err_to_name(ret));
        return;
    }
    if (done) finish_browser_load();
}

static void show_browser()
{
    (void)stop_midi_audio(true, "back_to_list");
    (void)stop_nsf_audio(true, "back_to_list");
    cancel_midi_player();
    cancel_nsf_player();
    g_page = VisualMusicPage::Browser;
    set_visible(g_browser_host, true);
    set_browser_header_visible(true);
    set_visible(g_player_host, false);
    update_header();
    update_rows();
    gesture_router_reset();
}

static void show_launcher()
{
    if (g_root == nullptr || g_page != VisualMusicPage::Browser || g_current_dir == nullptr ||
        strcmp(g_current_dir, VisualMusicBrowser::kRootDirectory) != 0) {
        return;
    }
    const esp_err_t ret = app_launcher_overlay_show(g_root, AppId::Nsf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "电子音流圆环Launcher展开失败：%s", esp_err_to_name(ret));
    }
}

static void go_back()
{
    if (g_page != VisualMusicPage::Browser) {
        show_browser();
        lv_obj_invalidate(g_root);
        return;
    }

    if (g_current_dir == nullptr ||
        strcmp(g_current_dir, VisualMusicBrowser::kRootDirectory) == 0) {
        show_launcher();
        return;
    }

    if (VisualMusicBrowser::parent_path(
            g_current_dir,
            g_scratch_path,
            VisualMusicBrowser::kPathBytes) != ESP_OK) {
        return;
    }
    snprintf(g_current_dir, VisualMusicBrowser::kPathBytes, "%s", g_scratch_path);
    g_selected_index = SIZE_MAX;
    update_header();
    begin_browser_load();
}

static void row_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click() || g_page != VisualMusicPage::Browser ||
        g_browser_load.phase != BrowserLoadPhase::Idle) {
        return;
    }

    const uintptr_t encoded = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (encoded == 0U) return;
    const size_t slot = static_cast<size_t>(encoded - 1U);
    const size_t index = g_first_index + slot;
    const VisualMusicBrowser::EntryIndex *entry =
        VisualMusicBrowser::entry_at(&g_directory, index);
    const char *name = VisualMusicBrowser::entry_name(&g_directory, index);
    if (entry == nullptr || name == nullptr ||
        VisualMusicBrowser::join_child_path(
            g_current_dir,
            name,
            g_scratch_path,
            VisualMusicBrowser::kPathBytes) != ESP_OK) {
        return;
    }

    if (VisualMusicBrowser::entry_is_directory(entry)) {
        snprintf(g_current_dir, VisualMusicBrowser::kPathBytes, "%s", g_scratch_path);
        g_selected_index = SIZE_MAX;
        update_header();
        begin_browser_load();
        return;
    }

    g_selected_index = index;
    snprintf(g_selected_path, VisualMusicBrowser::kPathBytes, "%s", g_scratch_path);
    if (VisualMusicBrowser::entry_is_midi(entry)) begin_midi_load();
    else begin_nsf_load();
}

static void back_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click()) {
        return;
    }
    go_back();
}

static void timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_root == nullptr || app_manager_foreground() != AppId::Nsf) return;

    browser_load_tick();
    midi_result_tick();
    nsf_result_tick();
    if (g_page == VisualMusicPage::MidiWaterfall && g_midi_audio_active) {
        const uint32_t now_tick = static_cast<uint32_t>(lv_tick_get());
        AudioMidiClockSnapshot clock = {};
        if (audio_service_midi_get_clock(&clock) && clock.active) {
            g_midi_paused = clock.paused || clock.eof;
            if (clock.eof) {
                // EOF 只消费一次，避免10ms UI timer在最后一曲或异步Restart尚未处理时重复入队。
                if (!g_midi_eof_action_handled) {
                    g_midi_eof_action_handled = true;
                    if (g_loop_mode == PlayerLoopMode::RepeatOne) {
                        if (audio_service_midi_restart(false)) {
                            g_midi_paused = false;
                            g_last_waterfall_draw_tick = 0U;
                        } else {
                            g_midi_eof_action_handled = false;
                        }
                    } else if (select_adjacent_track(
                                   +1,
                                   g_loop_mode == PlayerLoopMode::RepeatAll)) {
                        return;
                    }
                    update_midi_time_label();
                    update_player_controls();
                    if (g_waterfall_widget != nullptr) lv_obj_invalidate(g_waterfall_widget);
                }
            } else {
                g_midi_eof_action_handled = false;
                if (now_tick - g_last_time_label_tick >= kWaterfallTimeLabelPeriodMs) {
                    g_last_time_label_tick = now_tick;
                    update_midi_time_label();
                }
                if (!clock.paused &&
                    (g_last_waterfall_draw_tick == 0U ||
                     now_tick - g_last_waterfall_draw_tick >= kWaterfallFramePeriodMs)) {
                    g_last_waterfall_draw_tick = now_tick;
                    if (g_waterfall_widget != nullptr) lv_obj_invalidate(g_waterfall_widget);
                }
            }
        }
    }

    if (g_page == VisualMusicPage::NsfReady && g_nsf_audio_active) {
        const uint32_t now_tick = static_cast<uint32_t>(lv_tick_get());
        AudioNsfClockSnapshot clock = {};
        if (audio_service_nsf_get_clock(&clock) && clock.active) {
            const bool paused_changed = g_nsf_paused != (clock.paused || clock.eof);
            g_nsf_paused = clock.paused || clock.eof;
            g_nsf_eof = clock.eof;
            g_nsf_failed = clock.failed;
            if (clock.track < g_nsf_image.track_count) g_nsf_track = clock.track;
            if (clock.failed) {
                (void)stop_nsf_audio(true, "nsf_runtime_failed");
                g_nsf_failed = true;
                update_nsf_ready_ui();
            } else if (clock.eof) {
                // NSF v1 没有 Track 时长：AudioTask 依据持续静音或3分钟附近完整循环边界发布 EOF。
                // UI 只消费一次 EOF，并按当前循环模式切换 Subsong。
                if (!g_nsf_eof_action_handled) {
                    g_nsf_eof_action_handled = true;
                    if (g_loop_mode == PlayerLoopMode::RepeatOne) {
                        if (audio_service_nsf_set_track(g_nsf_track, false)) {
                            g_nsf_paused = false;
                            g_nsf_eof = false;
                        } else {
                            g_nsf_eof_action_handled = false;
                        }
                    } else if (select_nsf_subsong(
                                   +1,
                                   g_loop_mode == PlayerLoopMode::RepeatAll)) {
                        return;
                    }
                    update_nsf_time_label();
                    update_player_controls();
                }
            } else {
                g_nsf_eof_action_handled = false;
                if (g_last_nsf_time_label_tick == 0U ||
                    now_tick - g_last_nsf_time_label_tick >= kWaterfallTimeLabelPeriodMs) {
                    g_last_nsf_time_label_tick = now_tick;
                    update_nsf_time_label();
                    if (paused_changed) update_player_controls();
                }
            }
        }
    }

    UiGestureAction action = UiGestureAction::None;
    if (!gesture_router_take_action(&action)) return;
    if (app_launcher_overlay_is_visible()) {
        app_launcher_overlay_hide();
        return;
    }

    switch (action) {
        case UiGestureAction::SwipeRight:
            go_back();
            break;
        case UiGestureAction::SwipeUpTrack:
            if (g_page == VisualMusicPage::Browser) shift_window(+1);
            break;
        case UiGestureAction::SwipeDownTrack:
            if (g_page == VisualMusicPage::Browser) shift_window(-1);
            break;
        case UiGestureAction::PullUpFromBottom:
            if (g_page == VisualMusicPage::Browser) show_launcher();
            break;
        default:
            break;
    }
}

static esp_err_t create_rows()
{
    if (g_browser_host == nullptr) return ESP_ERR_INVALID_STATE;
    for (size_t slot = 0U; slot < kVisibleRows; ++slot) {
        RowUi &ui = g_rows[slot];
        ui.row = lv_button_create(g_browser_host);
        if (ui.row == nullptr) return ESP_ERR_NO_MEM;
        ui_common_lock_object(ui.row);
        lv_obj_set_pos(
            ui.row,
            0,
            static_cast<int32_t>(slot) * (kRowHeight + kRowGap));
        lv_obj_set_size(ui.row, LV_PCT(100), kRowHeight);
        lv_obj_set_style_radius(ui.row, 12, 0);
        lv_obj_set_style_border_width(ui.row, 0, 0);
        lv_obj_set_style_bg_opa(ui.row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(ui.row, 16, 0);
        lv_obj_set_style_pad_right(ui.row, 14, 0);
        lv_obj_add_event_cb(
            ui.row,
            row_clicked_cb,
            LV_EVENT_CLICKED,
            reinterpret_cast<void *>(static_cast<uintptr_t>(slot + 1U)));

        ui.name = make_label(ui.row, "", 0xEEF1F5, LV_TEXT_ALIGN_LEFT);
        if (ui.name == nullptr) return ESP_ERR_NO_MEM;
        lv_obj_set_width(ui.name, 330);
        lv_label_set_long_mode(ui.name, LV_LABEL_LONG_DOT);
        lv_obj_align(ui.name, LV_ALIGN_LEFT_MID, 0, 0);

        ui.kind = make_label(ui.row, "", 0x677385, LV_TEXT_ALIGN_RIGHT);
        if (ui.kind == nullptr) return ESP_ERR_NO_MEM;
        lv_obj_set_width(ui.kind, 70);
        lv_obj_align(ui.kind, LV_ALIGN_RIGHT_MID, 0, 0);
        set_visible(ui.row, false);
    }

    g_browser_position = make_label(g_browser_host, "", 0x697687, LV_TEXT_ALIGN_CENTER);
    if (g_browser_position == nullptr) return ESP_ERR_NO_MEM;
    lv_obj_set_width(g_browser_position, LV_PCT(100));
    lv_obj_align(g_browser_position, LV_ALIGN_BOTTOM_MID, 0, -2);
    set_visible(g_browser_position, false);
    return ESP_OK;
}

static esp_err_t cleanup_create_failure(esp_err_t err)
{
    cancel_browser_load();
    (void)stop_midi_audio(true, "create_failure");
    cancel_midi_player();
    cancel_nsf_player();
    VisualMusicBrowser::release_directory(&g_directory);
    if (g_timer != nullptr) {
        lv_timer_delete(g_timer);
        g_timer = nullptr;
    }
    if (g_root != nullptr) {
        lv_obj_delete(g_root);
        g_root = nullptr;
    }
    g_header_back = nullptr;
    g_header_title = nullptr;
    g_header_line = nullptr;
    g_browser_host = nullptr;
    g_browser_status = nullptr;
    g_browser_position = nullptr;
    for (RowUi &ui : g_rows) ui = {};
    g_player_host = nullptr;
    g_player_format = nullptr;
    g_player_title = nullptr;
    g_player_message = nullptr;
    g_player_hint = nullptr;
    g_waterfall_widget = nullptr;
    g_player_time = nullptr;
    g_player_controls = nullptr;
    g_loop_button = nullptr;
    g_loop_label = nullptr;
    g_prev_button = nullptr;
    g_play_button = nullptr;
    g_play_label = nullptr;
    g_next_button = nullptr;
    g_list_button = nullptr;
    if (g_current_dir != nullptr) heap_caps_free(g_current_dir);
    if (g_scratch_path != nullptr) heap_caps_free(g_scratch_path);
    if (g_selected_path != nullptr) heap_caps_free(g_selected_path);
    g_current_dir = nullptr;
    g_scratch_path = nullptr;
    g_selected_path = nullptr;
    g_page = VisualMusicPage::Browser;
    return err;
}

static esp_err_t visual_music_create()
{
    if (g_root != nullptr) return ESP_OK;

    g_current_dir = static_cast<char *>(heap_caps_calloc(
        VisualMusicBrowser::kPathBytes,
        1U,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_scratch_path = static_cast<char *>(heap_caps_calloc(
        VisualMusicBrowser::kPathBytes,
        1U,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_selected_path = static_cast<char *>(heap_caps_calloc(
        VisualMusicBrowser::kPathBytes,
        1U,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_current_dir == nullptr || g_scratch_path == nullptr || g_selected_path == nullptr) {
        return cleanup_create_failure(ESP_ERR_NO_MEM);
    }
    snprintf(
        g_current_dir,
        VisualMusicBrowser::kPathBytes,
        "%s",
        VisualMusicBrowser::kRootDirectory);

    g_root = lv_obj_create(lv_screen_active());
    if (g_root == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    ui_common_lock_object(g_root);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_size(g_root, 460, 460);
    lv_obj_set_style_radius(g_root, 0, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x0A0D12), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);

    g_header_back = lv_button_create(g_root);
    if (g_header_back == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    ui_common_lock_object(g_header_back);
    lv_obj_set_pos(g_header_back, 12, 14);
    lv_obj_set_size(g_header_back, 48, 40);
    lv_obj_set_style_radius(g_header_back, 12, 0);
    lv_obj_set_style_bg_color(g_header_back, lv_color_hex(0x18202B), 0);
    lv_obj_set_style_bg_opa(g_header_back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_header_back, 0, 0);
    lv_obj_set_ext_click_area(g_header_back, 10);
    lv_obj_add_event_cb(g_header_back, back_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *back_label = make_label(g_header_back, "<", 0xF0F3F7, LV_TEXT_ALIGN_CENTER);
    if (back_label == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    lv_obj_center(back_label);

    g_header_title = make_label(g_root, "电子音流", 0xF2F4F7, LV_TEXT_ALIGN_CENTER);
    if (g_header_title == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    lv_obj_set_width(g_header_title, 300);
    lv_label_set_long_mode(g_header_title, LV_LABEL_LONG_DOT);
    lv_obj_align(g_header_title, LV_ALIGN_TOP_MID, 0, 20);

    g_header_line = lv_obj_create(g_root);
    if (g_header_line == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    ui_common_lock_object(g_header_line);
    lv_obj_set_size(g_header_line, 390, 1);
    lv_obj_align(g_header_line, LV_ALIGN_TOP_MID, 0, kHeaderHeight - 1);
    lv_obj_set_style_border_width(g_header_line, 0, 0);
    lv_obj_set_style_bg_color(g_header_line, lv_color_hex(0x1C2430), 0);
    lv_obj_set_style_bg_opa(g_header_line, LV_OPA_COVER, 0);

    g_browser_host = lv_obj_create(g_root);
    if (g_browser_host == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    ui_common_lock_object(g_browser_host);
    lv_obj_set_pos(g_browser_host, kContentMargin, kHeaderHeight + 8);
    lv_obj_set_size(
        g_browser_host,
        460 - kContentMargin * 2,
        460 - (kHeaderHeight + 8) - kContentMargin);
    lv_obj_set_style_radius(g_browser_host, 0, 0);
    lv_obj_set_style_border_width(g_browser_host, 0, 0);
    lv_obj_set_style_bg_opa(g_browser_host, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(g_browser_host, 0, 0);
    lv_obj_remove_flag(g_browser_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_browser_host, LV_SCROLLBAR_MODE_OFF);
    const esp_err_t row_ret = create_rows();
    if (row_ret != ESP_OK) return cleanup_create_failure(row_ret);

    g_player_host = lv_obj_create(g_root);
    if (g_player_host == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    ui_common_lock_object(g_player_host);
    lv_obj_set_pos(g_player_host, 0, 0);
    lv_obj_set_size(g_player_host, 460, 460);
    lv_obj_set_style_radius(g_player_host, 0, 0);
    lv_obj_set_style_border_width(g_player_host, 0, 0);
    lv_obj_set_style_bg_color(g_player_host, lv_color_hex(0x0A0D12), 0);
    lv_obj_set_style_bg_opa(g_player_host, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_player_host, 0, 0);
    lv_obj_remove_flag(g_player_host, LV_OBJ_FLAG_SCROLLABLE);

    // 播放页固定为三段式：标题栏 / 全宽瀑布流 / 操作栏，不再使用居中卡片窗口。
    lv_obj_t *title_bar = lv_obj_create(g_player_host);
    if (title_bar == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    ui_common_lock_object(title_bar);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_size(title_bar, 460, kPlayerTitleHeight);
    lv_obj_set_style_radius(title_bar, 0, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_set_style_bg_color(title_bar, lv_color_hex(0x0A0D12), 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(title_bar, 0, 0);
    lv_obj_remove_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

    g_player_title = make_label(title_bar, "", 0xF2F4F7, LV_TEXT_ALIGN_CENTER);
    g_player_format = make_label(title_bar, "", 0x7E8A99, LV_TEXT_ALIGN_LEFT);
    g_player_time = make_label(title_bar, "", 0x8E9AAA, LV_TEXT_ALIGN_RIGHT);
    if (g_player_title == nullptr || g_player_format == nullptr || g_player_time == nullptr) {
        return cleanup_create_failure(ESP_ERR_NO_MEM);
    }
    lv_obj_set_width(g_player_title, 420);
    lv_label_set_long_mode(g_player_title, LV_LABEL_LONG_DOT);
    lv_obj_align(g_player_title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_width(g_player_format, 180);
    lv_label_set_long_mode(g_player_format, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_player_format, 14, 39);
    lv_obj_set_width(g_player_time, 240);
    lv_obj_set_pos(g_player_time, 206, 39);

    lv_obj_t *title_line = lv_obj_create(title_bar);
    if (title_line == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    ui_common_lock_object(title_line);
    lv_obj_set_size(title_line, 430, 1);
    lv_obj_align(title_line, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_width(title_line, 0, 0);
    lv_obj_set_style_bg_color(title_line, lv_color_hex(0x1C2430), 0);
    lv_obj_set_style_bg_opa(title_line, LV_OPA_COVER, 0);

    g_waterfall_widget = lv_obj_create(g_player_host);
    if (g_waterfall_widget == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    ui_common_lock_object(g_waterfall_widget);
    lv_obj_set_pos(g_waterfall_widget, 0, kPlayerTitleHeight);
    lv_obj_set_size(g_waterfall_widget, 460, kPlayerWaterfallHeight);
    lv_obj_set_style_radius(g_waterfall_widget, 0, 0);
    lv_obj_set_style_border_width(g_waterfall_widget, 0, 0);
    lv_obj_set_style_bg_color(g_waterfall_widget, lv_color_hex(0x080C12), 0);
    lv_obj_set_style_bg_opa(g_waterfall_widget, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_waterfall_widget, 0, 0);
    lv_obj_remove_flag(g_waterfall_widget, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_waterfall_widget, waterfall_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

    g_player_message = make_label(g_player_host, "", 0xBFC9D6, LV_TEXT_ALIGN_CENTER);
    g_player_hint = make_label(g_player_host, "", 0x7F8A99, LV_TEXT_ALIGN_CENTER);
    if (g_player_message == nullptr || g_player_hint == nullptr) {
        return cleanup_create_failure(ESP_ERR_NO_MEM);
    }
    lv_obj_set_width(g_player_message, 390);
    lv_label_set_long_mode(g_player_message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(g_player_message, 8, 0);
    lv_obj_align(g_player_message, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_set_width(g_player_hint, 390);
    lv_label_set_long_mode(g_player_hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(g_player_hint, LV_ALIGN_TOP_MID, 0, 232);

    g_player_controls = lv_obj_create(g_player_host);
    if (g_player_controls == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    ui_common_lock_object(g_player_controls);
    lv_obj_set_pos(g_player_controls, 0, kPlayerTitleHeight + kPlayerWaterfallHeight);
    lv_obj_set_size(g_player_controls, 460, kPlayerControlHeight);
    lv_obj_set_style_radius(g_player_controls, 0, 0);
    lv_obj_set_style_border_width(g_player_controls, 0, 0);
    lv_obj_set_style_bg_color(g_player_controls, lv_color_hex(0x0D1219), 0);
    lv_obj_set_style_bg_opa(g_player_controls, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_player_controls, 0, 0);
    lv_obj_remove_flag(g_player_controls, LV_OBJ_FLAG_SCROLLABLE);

    g_loop_button = make_control_button(g_player_controls, 58, LV_SYMBOL_RIGHT, true, &g_loop_label);
    g_prev_button = make_control_button(g_player_controls, 58, LV_SYMBOL_PREV, true, nullptr);
    g_play_button = make_control_button(g_player_controls, 68, LV_SYMBOL_PLAY, true, &g_play_label);
    g_next_button = make_control_button(g_player_controls, 58, LV_SYMBOL_NEXT, true, nullptr);
    g_list_button = make_control_button(g_player_controls, 58, LV_SYMBOL_LIST, true, nullptr);
    if (g_loop_button == nullptr || g_prev_button == nullptr || g_play_button == nullptr ||
        g_next_button == nullptr || g_list_button == nullptr) {
        return cleanup_create_failure(ESP_ERR_NO_MEM);
    }
    lv_obj_align(g_loop_button, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_align(g_prev_button, LV_ALIGN_LEFT_MID, 104, 0);
    lv_obj_align(g_play_button, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(g_next_button, LV_ALIGN_RIGHT_MID, -104, 0);
    lv_obj_align(g_list_button, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_opa(g_play_button, 220, 0);
    if (g_play_label != nullptr) lv_obj_set_style_text_color(g_play_label, lv_color_hex(0x111111), 0);
    lv_obj_add_event_cb(g_loop_button, loop_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g_prev_button, prev_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g_play_button, play_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g_next_button, next_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g_list_button, list_clicked_cb, LV_EVENT_CLICKED, nullptr);

    set_visible(g_waterfall_widget, false);
    set_visible(g_player_time, false);
    set_visible(g_player_host, false);
    update_player_controls();

    g_timer = lv_timer_create(timer_cb, kBrowserTimerPeriodMs, nullptr);
    if (g_timer == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    lv_timer_pause(g_timer);
    set_visible(g_root, false);

    ESP_LOGI(TAG, "电子音流 create完成：5-Row Browser + 三段式MIDI Waterfall Player");
    return ESP_OK;
}

static esp_err_t visual_music_enter()
{
    if (g_root == nullptr || g_current_dir == nullptr) return ESP_ERR_INVALID_STATE;

    g_midi_audio_active = false;
    g_music_paused_for_midi = false;
    g_midi_eof_action_handled = false;
    g_nsf_audio_active = false;
    g_nsf_paused = true;
    g_nsf_failed = false;
    g_music_paused_for_nsf = false;
    cancel_midi_player();
    cancel_nsf_player();
    g_page = VisualMusicPage::Browser;
    g_selected_path[0] = '\0';
    g_selected_index = SIZE_MAX;
    snprintf(
        g_current_dir,
        VisualMusicBrowser::kPathBytes,
        "%s",
        VisualMusicBrowser::kRootDirectory);
    set_visible(g_root, true);
    set_visible(g_browser_host, true);
    set_browser_header_visible(true);
    set_visible(g_player_host, false);
    update_header();
    gesture_router_reset();
    if (g_timer != nullptr) {
        lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
        lv_timer_resume(g_timer);
    }
    begin_browser_load();
    ESP_LOGI(
        TAG,
        "电子音流进入Foreground：/synth 浏览器已显示；MIDI使用GM-Lite Synth，NSF使用6502/2A03基础5通道");
    return ESP_OK;
}

static esp_err_t visual_music_leave(AppRunState next_state)
{
    (void)next_state;
    cancel_browser_load();
    if (!stop_midi_audio(true, "app_leave")) return ESP_ERR_INVALID_STATE;
    if (!stop_nsf_audio(true, "app_leave")) return ESP_ERR_INVALID_STATE;
    cancel_midi_player();
    cancel_nsf_player();
    if (g_timer != nullptr) lv_timer_pause(g_timer);
    gesture_router_reset();
    app_launcher_overlay_hide();
    set_visible(g_root, false);
    g_page = VisualMusicPage::Browser;
    ESP_LOGI(TAG, "电子音流离开Foreground：MIDI/NSF音频与目录协作任务已停止，进入前Music状态已恢复");
    return ESP_OK;
}

static void visual_music_destroy()
{
    cancel_browser_load();
    (void)stop_midi_audio(true, "app_destroy");
    (void)stop_nsf_audio(true, "app_destroy");
    cancel_midi_player();
    cancel_nsf_player();
    VisualMusicBrowser::release_directory(&g_directory);
    if (g_timer != nullptr) {
        lv_timer_delete(g_timer);
        g_timer = nullptr;
    }
    app_launcher_overlay_destroy();

    g_header_back = nullptr;
    g_header_title = nullptr;
    g_header_line = nullptr;
    g_browser_host = nullptr;
    g_browser_status = nullptr;
    g_browser_position = nullptr;
    for (RowUi &ui : g_rows) ui = {};
    g_player_host = nullptr;
    g_player_format = nullptr;
    g_player_title = nullptr;
    g_player_message = nullptr;
    g_player_hint = nullptr;
    g_waterfall_widget = nullptr;
    g_player_time = nullptr;
    g_player_controls = nullptr;
    g_loop_button = nullptr;
    g_loop_label = nullptr;
    g_prev_button = nullptr;
    g_play_button = nullptr;
    g_play_label = nullptr;
    g_next_button = nullptr;
    g_list_button = nullptr;
    if (g_root != nullptr) {
        lv_obj_delete(g_root);
        g_root = nullptr;
    }

    if (g_current_dir != nullptr) heap_caps_free(g_current_dir);
    if (g_scratch_path != nullptr) heap_caps_free(g_scratch_path);
    if (g_selected_path != nullptr) heap_caps_free(g_selected_path);
    g_current_dir = nullptr;
    g_scratch_path = nullptr;
    g_selected_path = nullptr;
    g_first_index = 0U;
    g_selected_index = SIZE_MAX;
    g_loop_mode = PlayerLoopMode::Sequential;
    g_midi_audio_active = false;
    g_music_paused_for_midi = false;
    g_midi_eof_action_handled = false;
    g_nsf_audio_active = false;
    g_nsf_paused = true;
    g_nsf_failed = false;
    g_music_paused_for_nsf = false;
    g_page = VisualMusicPage::Browser;
    ESP_LOGI(TAG, "电子音流 destroy完成：LVGL/目录索引/路径PSRAM已释放");
}

} // namespace

esp_err_t visual_music_app_register()
{
    const esp_err_t midi_ret = VisualMusicMidi::init();
    if (midi_ret != ESP_OK) return midi_ret;
    const esp_err_t nsf_ret = VisualMusicNsf::init();
    if (nsf_ret != ESP_OK) return nsf_ret;

    AppDescriptor descriptor = {};
    descriptor.id = AppId::Nsf;
    descriptor.name = "电子音流";
    descriptor.supports_background = false;
    descriptor.lifecycle.create = visual_music_create;
    descriptor.lifecycle.enter = visual_music_enter;
    descriptor.lifecycle.leave = visual_music_leave;
    descriptor.lifecycle.destroy = visual_music_destroy;

    const esp_err_t ret = app_manager_register(descriptor);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "电子音流 APP已注册：MIDI Synth/Waterfall + NSF 6502/2A03 + 统一AppManager生命周期");
    }
    return ret;
}
