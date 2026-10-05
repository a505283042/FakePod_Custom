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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "font/font_manager.h"
#include "gesture/gesture_router.h"
#include "lvgl.h"
#include "media_library.h"
#include "player_state.h"
#include "player_transport.h"
#include "ui_common.h"
#include "visual_music_browser_model.h"
#include "visual_music_nsf.h"

static const char *TAG = "电子音流";

namespace
{

enum class VisualMusicPage : uint8_t
{
    Browser = 0,
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
static constexpr uint32_t kBrowserTimerPeriodMs = 20U;
static constexpr uint32_t kNsfAnalyzingTimerPeriodMs = 20U;
static constexpr uint32_t kNsfFinalTimerPeriodMs = 10U;
static constexpr uint32_t kFlacSafePercent = 90U;
static constexpr size_t kScanBatchNoFlac = 8U;
static constexpr size_t kScanBatchWithFlac = 1U;
static constexpr uint32_t kWaitLogIntervalMs = 1000U;
// R46.0.47：时长分析期回退到约8.3fps，优先保证Sequencer/Timeline；Final仍保持PSRAM双帧25fps。
static constexpr uint32_t kNsfWaterfallAnalyzingFramePeriodMs = 120U; // ~8.3fps，分析期统一限速
static constexpr uint32_t kNsfWaterfallFinalFramePeriodMs = 50U;      // 20fps fallback
static constexpr uint32_t kNsfWaterfallDoubleBufferFinalFramePeriodMs = 40U; // 25fps
static constexpr uint32_t kNsfVisualSnapshotHoldMs = 300U;
static constexpr uint32_t kWaterfallTimeLabelPeriodMs = 250U;
// R46.0.49：播放页隐藏超过2s后释放557KiB双帧；短暂返回列表则直接复用，避免频繁申请。
static constexpr uint32_t kWaterfallHiddenReleaseDelayMs = 2000U;
// R46.0.51：Loader有界等待；2A03启动移出LVGL线程，避免慢INIT冻结返回手势。
static constexpr uint32_t kNsfLoadTimeoutMs = 8000U;
static constexpr uint32_t kNsfStartTaskStack = 3072U;
static constexpr UBaseType_t kNsfStartTaskPriority = 1U;
static constexpr BaseType_t kNsfStartTaskCore = 0;
// R46.0.41：恢复 R46.0.38 的钢琴窗时间范围，降低音符屏幕移动速度。
static constexpr uint32_t kWaterfallNsfFutureMs = 2000U;
static constexpr uint32_t kWaterfallNsfPastMs = 1800U;  // 历史区恢复到原来的1.8s
static constexpr int32_t kWaterfallPastPixels = 70;   // 播放线距控件底部的历史区高度
static constexpr uint8_t kWaterfallPitchMin = 36U; // C2以下统一折叠到左侧低频区
static constexpr uint8_t kWaterfallPitchMax = 96U; // C7以上夹到最右列
static constexpr int32_t kWaterfallLowAreaWidth = 44;
static constexpr size_t kNsfVisualWindowCapacity = 512U; // 恢复原钢琴窗密度，缓存位于PSRAM
static constexpr int32_t kPlayerTitleHeight = 68;
static constexpr int32_t kPlayerControlHeight = 82;
static constexpr int32_t kPlayerWaterfallHeight = 460 - kPlayerTitleHeight - kPlayerControlHeight;
static constexpr int32_t kWaterfallFrameWidth = 460;
static constexpr int32_t kWaterfallFrameHeight = kPlayerWaterfallHeight;
static constexpr size_t kWaterfallFramePixels =
    static_cast<size_t>(kWaterfallFrameWidth) * kWaterfallFrameHeight;
static constexpr size_t kWaterfallFrameBytes = kWaterfallFramePixels * sizeof(uint16_t);

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

struct NsfStartTaskArgs
{
    uint8_t *prg = nullptr; // PSRAM；worker提交给AudioService后即可释放
    size_t prg_size = 0U;
    NsfSynthConfig config = {};
    uint32_t generation = 0U;
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
static lv_obj_t *g_waterfall_frame_image = nullptr;
static uint16_t *g_waterfall_frames[2] = {nullptr, nullptr};
static lv_image_dsc_t g_waterfall_frame_dsc[2] = {};
static uint8_t g_waterfall_front_index = 0U;
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
static VisualMusicNsf::Image g_nsf_image = {};
static uint8_t g_nsf_track = 0U;
static AudioNsfVisualEvent *g_nsf_visual_window = nullptr;
static uint32_t g_last_waterfall_draw_tick = 0U;
static size_t g_nsf_visual_window_count = 0U;
static uint32_t g_last_nsf_visual_snapshot_tick = 0U;
static bool g_nsf_final_high_fps = false;
static bool g_waterfall_hidden_release_pending = false;
static uint32_t g_waterfall_hidden_since_tick = 0U;
static bool g_nsf_paused = true;
static bool g_nsf_audio_active = false;
static bool g_nsf_eof = false;
// NSF EOF 按 AudioTask Track 实例 revision 去重，和主 Music playback_revision 采用同一模型。
// 同一 Subsong RepeatOne reset 后 track 不变，但 revision 会变化，因此不会吞掉新实例的 EOF。
static uint32_t g_last_nsf_eof_revision = 0U;
static bool g_nsf_failed = false;
static uint32_t g_nsf_load_started_tick = 0U;
// NSF Start worker只等待AudioTask；绝不触碰LVGL。结果由timer线程收口。
static portMUX_TYPE g_nsf_start_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_nsf_start_generation = 1U;
static uint32_t g_nsf_start_active_generation = 0U;
static uint32_t g_nsf_start_result_generation = 0U;
static bool g_nsf_start_task_active = false;
static bool g_nsf_start_result_pending = false;
static bool g_nsf_start_result_success = false;
static bool g_nsf_start_deferred = false;
static bool g_music_paused_for_nsf = false;
static bool g_music_deep_suspended_for_nsf = false;
static uint32_t g_last_nsf_time_label_tick = 0U;
static PlayerLoopMode g_loop_mode = PlayerLoopMode::Sequential;

// R46.0.50：BLE HostTask只提交NSF媒体意图；真正状态切换由电子音流timer串行执行。
static portMUX_TYPE g_remote_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_remote_context_active = false;
static NsfRemoteCommand g_remote_pending = NsfRemoteCommand::TogglePlayPause;
static bool g_remote_pending_valid = false;
static uint32_t g_remote_playback_revision = 1U;
static uint32_t g_remote_metadata_revision = 1U;
static uint32_t g_remote_track_index = UINT32_MAX;
static char g_remote_title[128] = {};
static char g_remote_artist[96] = {};

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
    const bool nsf_playing =
        g_page == VisualMusicPage::NsfReady && g_nsf_audio_active &&
        !g_nsf_paused && !g_nsf_eof && !g_nsf_failed;
    if (g_play_label != nullptr) {
        lv_label_set_text(g_play_label, nsf_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    if (g_play_button != nullptr) {
        const bool nsf_supported =
            g_page == VisualMusicPage::NsfReady && g_nsf_image.version == 1U &&
            g_nsf_image.track_count > 0U && g_nsf_image.expansion_chips == 0U &&
            (g_nsf_image.pal_ntsc_bits & 0x03U) != 0x01U;
        if (nsf_supported) {
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

static void remote_bump_revision(uint32_t *revision)
{
    if (revision == nullptr) return;
    ++(*revision);
    if (*revision == 0U) ++(*revision);
}

static void remote_build_identity(
    char *title, size_t title_size,
    char *artist, size_t artist_size,
    uint32_t *track_index)
{
    if (title == nullptr || title_size == 0U || artist == nullptr || artist_size == 0U ||
        track_index == nullptr) return;

    *track_index = g_nsf_track;
    snprintf(
        title,
        title_size,
        "%s",
        g_nsf_image.song_name[0] != '\0' ? g_nsf_image.song_name : basename_of(g_selected_path));
    if (g_nsf_image.artist[0] != '\0' && strcmp(g_nsf_image.artist, "<?>") != 0) {
        snprintf(
            artist,
            artist_size,
            "%s · Track %u/%u",
            g_nsf_image.artist,
            static_cast<unsigned>(g_nsf_track + 1U),
            static_cast<unsigned>(g_nsf_image.track_count));
    } else {
        snprintf(
            artist,
            artist_size,
            "Track %u/%u",
            static_cast<unsigned>(g_nsf_track + 1U),
            static_cast<unsigned>(g_nsf_image.track_count));
    }
}

static void remote_context_begin()
{
    char title[sizeof(g_remote_title)] = {};
    char artist[sizeof(g_remote_artist)] = {};
    uint32_t track_index = UINT32_MAX;
    remote_build_identity(title, sizeof(title), artist, sizeof(artist), &track_index);

    portENTER_CRITICAL(&g_remote_mux);
    g_remote_context_active = true;
    g_remote_pending_valid = false;
    g_remote_track_index = track_index;
    memcpy(g_remote_title, title, sizeof(g_remote_title));
    memcpy(g_remote_artist, artist, sizeof(g_remote_artist));
    remote_bump_revision(&g_remote_playback_revision);
    remote_bump_revision(&g_remote_metadata_revision);
    portEXIT_CRITICAL(&g_remote_mux);
}

static void remote_identity_changed()
{
    char title[sizeof(g_remote_title)] = {};
    char artist[sizeof(g_remote_artist)] = {};
    uint32_t track_index = UINT32_MAX;
    remote_build_identity(title, sizeof(title), artist, sizeof(artist), &track_index);

    portENTER_CRITICAL(&g_remote_mux);
    if (g_remote_context_active) {
        g_remote_track_index = track_index;
        memcpy(g_remote_title, title, sizeof(g_remote_title));
        memcpy(g_remote_artist, artist, sizeof(g_remote_artist));
        remote_bump_revision(&g_remote_playback_revision);
        remote_bump_revision(&g_remote_metadata_revision);
    }
    portEXIT_CRITICAL(&g_remote_mux);
}

static void remote_playback_changed()
{
    portENTER_CRITICAL(&g_remote_mux);
    if (g_remote_context_active) remote_bump_revision(&g_remote_playback_revision);
    portEXIT_CRITICAL(&g_remote_mux);
}

static void remote_context_end()
{
    portENTER_CRITICAL(&g_remote_mux);
    if (g_remote_context_active) {
        g_remote_context_active = false;
        g_remote_pending_valid = false;
        g_remote_track_index = UINT32_MAX;
        g_remote_title[0] = '\0';
        g_remote_artist[0] = '\0';
        remote_bump_revision(&g_remote_playback_revision);
        remote_bump_revision(&g_remote_metadata_revision);
    }
    portEXIT_CRITICAL(&g_remote_mux);
}

static bool remote_take_pending(NsfRemoteCommand *out_command)
{
    if (out_command == nullptr) return false;
    bool have = false;
    portENTER_CRITICAL(&g_remote_mux);
    if (g_remote_pending_valid) {
        *out_command = g_remote_pending;
        g_remote_pending_valid = false;
        have = true;
    }
    portEXIT_CRITICAL(&g_remote_mux);
    return have;
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

static bool pause_music_for_nsf_exclusive()
{
    player_transport_cancel_deferred_nas_play("进入NSF独占播放");
    // 同一 NSF Session 内返回列表/换文件不恢复 Music；已经深度挂起时直接复用该上下文。
    if (g_music_deep_suspended_for_nsf || g_music_paused_for_nsf) return true;

    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        ESP_LOGI(TAG, "NSF Exclusive：AudioTask未就绪，无活动Music需要暂停");
        return true;
    }

    if (snapshot.state == AudioPlaybackState::Playing) {
        if (!audio_service_pause(true)) {
            ESP_LOGE(TAG, "NSF Exclusive：暂停Music失败");
            return false;
        }
        g_music_paused_for_nsf = true;
        if (!audio_service_get_snapshot(&snapshot)) {
            ESP_LOGW(TAG, "NSF Exclusive：暂停后读取Music状态失败，本次保持浅暂停");
            return true;
        }
    } else if (snapshot.state != AudioPlaybackState::Paused) {
        ESP_LOGI(TAG, "NSF Exclusive：Music当前非Playing/Paused(state=%u)，保持原状态",
            static_cast<unsigned>(snapshot.state));
        return true;
    }

    // R46.0.37：和 Video 共用 AudioTask 的 Deep Suspend 核心。
    // 进入电子音流列表时不触发；只有 NSF 已解析通过、真正准备启动 2A03 时才尝试。
    const size_t player_track = player_state_get_index();
    const char *path = player_state_get_path();
    MediaTechnicalInfo technical = {};
    const bool has_technical = media_library_get_technical_info(player_track, &technical);
    const bool mp3_index_safe = snapshot.format != MediaFormat::MP3 ||
        (has_technical && (technical.flags & MEDIA_TECH_PARSED) != 0U);
    const AudioPlaybackSource player_source = player_state_get_source() == PlayerMediaSource::Nas
        ? AudioPlaybackSource::NasHttp : AudioPlaybackSource::Local;
    const bool context_matches = player_state_is_ready() && path != nullptr && path[0] != '\0' &&
        player_track == snapshot.track_index && player_state_get_format() == snapshot.format &&
        player_source == snapshot.source;
    const bool nas_restart_from_beginning =
        snapshot.source == AudioPlaybackSource::NasHttp && snapshot.format == MediaFormat::MP3;
    const bool local_seek_restore = snapshot.source == AudioPlaybackSource::Local &&
        snapshot.seek_supported && mp3_index_safe;

    if ((nas_restart_from_beginning || local_seek_restore) && context_matches) {
        if (audio_service_music_deep_suspend(
                AudioMusicSuspendOwner::Nsf,
                snapshot.track_index,
                path,
                snapshot.format,
                has_technical ? &technical : nullptr,
                true)) {
            g_music_deep_suspended_for_nsf = true;
            ESP_LOGI(TAG,
                "NSF Exclusive：Music已深度挂起；source=%s restore=%s decoder/source/read-ahead/workspace已回收",
                nas_restart_from_beginning ? "NAS_HTTP" : "LOCAL",
                nas_restart_from_beginning ? "00:00" : "原位置");
        } else {
            ESP_LOGW(TAG, "NSF Exclusive：Music深度挂起不可用，本次保持浅暂停继续NSF");
        }
    } else {
        ESP_LOGI(TAG,
            "NSF Exclusive：Music保持浅暂停 source=%u seek=%u context=%u mp3_index=%u format=%s",
            static_cast<unsigned>(snapshot.source),
            static_cast<unsigned>(snapshot.seek_supported),
            static_cast<unsigned>(context_matches),
            static_cast<unsigned>(mp3_index_safe),
            media_format_name(snapshot.format));
    }

    ESP_LOGI(TAG, "NSF Exclusive：Music已暂停；AudioTask切换到6502/2A03");
    return true;
}

static bool stop_nsf_audio(bool restore_music, const char *reason, bool force_hardware_stop = false)
{
    if (g_nsf_audio_active || restore_music || force_hardware_stop) {
        // 深度挂起时 Music decoder 已不存在，NSF stop 只负责释放 NSF 自己的硬件；
        // 随后再由共用 Deep Restore 重建 Music。浅暂停路径仍沿用原硬件恢复。
        const bool restore_shallow_music_hardware = restore_music && !g_music_deep_suspended_for_nsf;
        if (!audio_service_nsf_stop(restore_shallow_music_hardware, true)) {
            ESP_LOGE(TAG, "NSF停止/恢复Music硬件失败：reason=%s",
                reason != nullptr ? reason : "unknown");
            return false;
        }
        g_nsf_audio_active = false;
        remote_context_end();
        g_nsf_paused = true;
        g_nsf_eof = false;
        g_last_nsf_eof_revision = 0U;
        g_nsf_failed = false;
        g_nsf_final_high_fps = false;
        g_nsf_visual_window_count = 0U;
        g_last_nsf_visual_snapshot_tick = 0U;
        if (g_timer != nullptr) lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
    }

    if (restore_music && g_music_deep_suspended_for_nsf) {
        if (!audio_service_music_deep_restore(AudioMusicSuspendOwner::Nsf, true)) {
            ESP_LOGW(TAG, "NSF Exclusive深度恢复Music失败：reason=%s；保留上下文等待生命周期重试",
                reason != nullptr ? reason : "unknown");
            return false;
        }
        g_music_deep_suspended_for_nsf = false;
    }

    // 只有进入 NSF 前原本是 Playing 才恢复播放；原本就是 Paused 则只重建到 Paused。
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

static uint32_t waterfall_level_color(uint32_t color, uint8_t level)
{
    const uint32_t scale = 62U + (static_cast<uint32_t>(level) * 38U) / 127U;
    const uint32_t r = (((color >> 16U) & 0xFFU) * scale) / 100U;
    const uint32_t g = (((color >> 8U) & 0xFFU) * scale) / 100U;
    const uint32_t b = ((color & 0xFFU) * scale) / 100U;
    return (r << 16U) | (g << 8U) | b;
}

// 历史区固定深色值：与各声部/通道基色同色系的深色，避免用缩放/半透明等效果。
static uint32_t waterfall_dim_color(uint32_t color)
{
    switch (color) {
        case 0x20C7F4: return 0x0D5A6E;   // 青
        case 0xFF5B9D: return 0x7A2B4A;   // 粉
        case 0x55A8FF: return 0x27527D;   // 蓝
        case 0x9A7CFF: return 0x46376F;   // 紫
        case 0xF4A62A: return 0x6E4A14;   // 橙
        default:       return 0x3A4250;   // 灰
    }
}

static uint32_t nsf_waterfall_color(AudioNsfVisualVoice voice)
{
    switch (voice) {
        case AudioNsfVisualVoice::Pulse1: return 0x20C7F4;
        case AudioNsfVisualVoice::Pulse2: return 0xFF5B9D;
        case AudioNsfVisualVoice::Triangle: return 0x55A8FF;
        case AudioNsfVisualVoice::Noise:
        case AudioNsfVisualVoice::Dmc: return 0xF4A62A;
        default: return 0xD7E4F5;
    }
}

static uint16_t waterfall_rgb565(uint32_t color)
{
    const uint8_t r = static_cast<uint8_t>((color >> 16U) & 0xFFU);
    const uint8_t g = static_cast<uint8_t>((color >> 8U) & 0xFFU);
    const uint8_t b = static_cast<uint8_t>(color & 0xFFU);
    return static_cast<uint16_t>(
        ((static_cast<uint16_t>(r) & 0xF8U) << 8U) |
        ((static_cast<uint16_t>(g) & 0xFCU) << 3U) |
        (static_cast<uint16_t>(b) >> 3U));
}

static void waterfall_frame_fill_rect(
    uint16_t *frame, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t color)
{
    if (frame == nullptr) return;
    if (x1 > x2 || y1 > y2 || x2 < 0 || y2 < 0 ||
        x1 >= kWaterfallFrameWidth || y1 >= kWaterfallFrameHeight) return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= kWaterfallFrameWidth) x2 = kWaterfallFrameWidth - 1;
    if (y2 >= kWaterfallFrameHeight) y2 = kWaterfallFrameHeight - 1;
    for (int32_t y = y1; y <= y2; ++y) {
        uint16_t *row = frame + static_cast<size_t>(y) * kWaterfallFrameWidth + x1;
        for (int32_t x = x1; x <= x2; ++x) *row++ = color;
    }
}

static void waterfall_frame_draw_grid(uint16_t *frame)
{
    if (frame == nullptr) return;
    const uint16_t bg = waterfall_rgb565(0x071425);
    for (size_t i = 0U; i < kWaterfallFramePixels; ++i) frame[i] = bg;

    const int32_t main_x1 = kWaterfallLowAreaWidth;
    const int32_t main_width = kWaterfallFrameWidth - main_x1;
    const int32_t pitch_span = kWaterfallPitchMax - kWaterfallPitchMin;
    for (uint8_t note = kWaterfallPitchMin; note <= kWaterfallPitchMax; ++note) {
        const int32_t x = main_x1 +
            (static_cast<int32_t>(note - kWaterfallPitchMin) * (main_width - 1)) / pitch_span;
        waterfall_frame_fill_rect(
            frame, x, 0, x, kWaterfallFrameHeight - 1,
            waterfall_rgb565((note % 12U) == 0U ? 0x263A55 : 0x16263A));
    }
    waterfall_frame_fill_rect(
        frame, main_x1 - 1, 0, main_x1, kWaterfallFrameHeight - 1,
        waterfall_rgb565(0x31475F));
    waterfall_frame_fill_rect(
        frame, kWaterfallLowAreaWidth / 2, 0, kWaterfallLowAreaWidth / 2,
        kWaterfallFrameHeight - 1, waterfall_rgb565(0x122033));
}

static void waterfall_frame_init_dsc(lv_image_dsc_t *dsc, const uint16_t *pixels)
{
    if (dsc == nullptr) return;
    *dsc = {};
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.flags = 0U;
    dsc->header.w = kWaterfallFrameWidth;
    dsc->header.h = kWaterfallFrameHeight;
    dsc->header.stride = static_cast<uint32_t>(kWaterfallFrameWidth) * 2U;
    dsc->data_size = static_cast<uint32_t>(kWaterfallFrameBytes);
    dsc->data = reinterpret_cast<const uint8_t *>(pixels);
}

static void waterfall_frame_buffers_release()
{
    for (size_t i = 0U; i < 2U; ++i) {
        if (g_waterfall_frames[i] != nullptr) heap_caps_free(g_waterfall_frames[i]);
        g_waterfall_frames[i] = nullptr;
        g_waterfall_frame_dsc[i] = {};
    }
    g_waterfall_front_index = 0U;
}

static bool waterfall_frame_buffers_prepare()
{
    waterfall_frame_buffers_release();
    for (size_t i = 0U; i < 2U; ++i) {
        g_waterfall_frames[i] = static_cast<uint16_t *>(heap_caps_malloc(
            kWaterfallFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (g_waterfall_frames[i] == nullptr) {
            waterfall_frame_buffers_release();
            return false;
        }
        waterfall_frame_draw_grid(g_waterfall_frames[i]);
        waterfall_frame_init_dsc(&g_waterfall_frame_dsc[i], g_waterfall_frames[i]);
    }
    g_waterfall_front_index = 0U;
    return true;
}

static size_t waterfall_refresh_visual_window(uint32_t now_ms)
{
    const uint32_t history_start = now_ms > kWaterfallNsfPastMs
        ? now_ms - kWaterfallNsfPastMs
        : 0U;
    const uint32_t future_end = UINT32_MAX - now_ms < kWaterfallNsfFutureMs
        ? UINT32_MAX
        : now_ms + kWaterfallNsfFutureMs;
    const size_t fresh_count = audio_service_nsf_copy_visual_events(
        g_nsf_track, history_start, future_end,
        g_nsf_visual_window, kNsfVisualWindowCapacity);
    const uint32_t snapshot_tick = lv_tick_get();
    if (fresh_count > 0U) {
        g_nsf_visual_window_count = fresh_count;
        g_last_nsf_visual_snapshot_tick = snapshot_tick;
    } else if (g_last_nsf_visual_snapshot_tick == 0U ||
               snapshot_tick - g_last_nsf_visual_snapshot_tick > kNsfVisualSnapshotHoldMs) {
        g_nsf_visual_window_count = 0U;
    }
    return g_nsf_visual_window_count;
}

static void waterfall_frame_draw_event(
    uint16_t *frame, int32_t current_line_y, int32_t future_pixels, int32_t past_pixels,
    uint32_t now_ms, const AudioNsfVisualEvent &note)
{
    if (frame == nullptr || note.end_ms < note.start_ms) return;
    auto time_to_y = [current_line_y, future_pixels, past_pixels, now_ms](uint32_t time_ms) -> int32_t {
        if (time_ms == now_ms) return current_line_y;
        if (time_ms < now_ms) {
            const uint64_t delta_ms = static_cast<uint64_t>(now_ms) - time_ms;
            if (delta_ms >= kWaterfallNsfPastMs) return current_line_y + past_pixels;
            return current_line_y + static_cast<int32_t>(
                delta_ms * static_cast<uint64_t>(past_pixels) / kWaterfallNsfPastMs);
        }
        const uint64_t delta_ms = static_cast<uint64_t>(time_ms) - now_ms;
        if (delta_ms >= kWaterfallNsfFutureMs) return current_line_y - future_pixels;
        return current_line_y - static_cast<int32_t>(
            delta_ms * static_cast<uint64_t>(future_pixels) / kWaterfallNsfFutureMs);
    };

    int32_t y1 = time_to_y(note.end_ms);
    int32_t y2 = time_to_y(note.start_ms);
    if (y1 > y2) { const int32_t swap = y1; y1 = y2; y2 = swap; }
    if (y2 - y1 < 3) y2 = y1 + 3;
    if (y2 < 0 || y1 >= kWaterfallFrameHeight) return;
    if (y1 < 0) y1 = 0;
    if (y2 >= kWaterfallFrameHeight) y2 = kWaterfallFrameHeight - 1;

    int8_t low_lane = -1;
    if (note.voice == AudioNsfVisualVoice::Noise) low_lane = 0;
    else if (note.voice == AudioNsfVisualVoice::Dmc || note.note < kWaterfallPitchMin) low_lane = 1;

    int32_t center_x = 0;
    int32_t note_width = 6;
    if (low_lane >= 0) {
        const int32_t lane_width = kWaterfallLowAreaWidth / 2;
        center_x = lane_width * low_lane + lane_width / 2;
        note_width = 9;
    } else {
        const int32_t main_x1 = kWaterfallLowAreaWidth;
        const int32_t main_width = kWaterfallFrameWidth - main_x1;
        uint8_t clamped_note = note.note;
        if (clamped_note < kWaterfallPitchMin) clamped_note = kWaterfallPitchMin;
        if (clamped_note > kWaterfallPitchMax) clamped_note = kWaterfallPitchMax;
        center_x = main_x1 +
            (static_cast<int32_t>(clamped_note - kWaterfallPitchMin) * (main_width - 1)) /
            (kWaterfallPitchMax - kWaterfallPitchMin);
    }
    int32_t x1 = center_x - note_width / 2;
    int32_t x2 = x1 + note_width - 1;

    const uint32_t base_color = low_lane >= 0 ? 0xF4A62A : nsf_waterfall_color(note.voice);
    const uint16_t bright = waterfall_rgb565(waterfall_level_color(base_color, note.level));
    const uint16_t dim = waterfall_rgb565(waterfall_dim_color(base_color));
    const int32_t bright_y2 = y2 < current_line_y ? y2 : current_line_y;
    if (bright_y2 >= y1) waterfall_frame_fill_rect(frame, x1, y1, x2, bright_y2, bright);
    const int32_t dim_y1 = y1 > current_line_y ? y1 : current_line_y;
    if (y2 >= dim_y1) waterfall_frame_fill_rect(frame, x1, dim_y1, x2, y2, dim);
}

static bool waterfall_render_psram_frame(uint32_t now_ms)
{
    if (g_waterfall_frame_image == nullptr || g_waterfall_frames[0] == nullptr ||
        g_waterfall_frames[1] == nullptr) return false;
    const uint8_t back = static_cast<uint8_t>(g_waterfall_front_index ^ 1U);
    uint16_t *frame = g_waterfall_frames[back];
    waterfall_frame_draw_grid(frame);

    const int32_t top = 4;
    const int32_t current_line_y = kWaterfallFrameHeight - 1 - kWaterfallPastPixels - 2;
    const int32_t future_pixels = current_line_y - top;
    const int32_t past_pixels = kWaterfallFrameHeight - 1 - current_line_y;
    const uint32_t history_start = now_ms > kWaterfallNsfPastMs
        ? now_ms - kWaterfallNsfPastMs : 0U;
    const uint32_t future_end = UINT32_MAX - now_ms < kWaterfallNsfFutureMs
        ? UINT32_MAX : now_ms + kWaterfallNsfFutureMs;
    const size_t count = waterfall_refresh_visual_window(now_ms);
    for (size_t i = 0U; i < count; ++i) {
        const AudioNsfVisualEvent &note = g_nsf_visual_window[i];
        if (note.end_ms < note.start_ms || note.end_ms < history_start ||
            note.start_ms > future_end) continue;
        waterfall_frame_draw_event(frame, current_line_y, future_pixels, past_pixels, now_ms, note);
    }
    waterfall_frame_fill_rect(
        frame, 2, current_line_y, kWaterfallFrameWidth - 3, current_line_y + 1,
        waterfall_rgb565(0x30D7FF));

    // lv_image_set_src()本身登记失效；不要再额外invalidate同一区域。
    lv_image_set_src(g_waterfall_frame_image, &g_waterfall_frame_dsc[back]);
    g_waterfall_front_index = back;
    return true;
}

static void waterfall_cancel_hidden_release()
{
    g_waterfall_hidden_release_pending = false;
    g_waterfall_hidden_since_tick = 0U;
}

static void waterfall_release_double_buffer(const char *reason)
{
    waterfall_cancel_hidden_release();
    if (g_waterfall_frame_image == nullptr &&
        g_waterfall_frames[0] == nullptr && g_waterfall_frames[1] == nullptr) {
        return;
    }

    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (g_waterfall_frame_image != nullptr) {
        lv_obj_delete(g_waterfall_frame_image);
        g_waterfall_frame_image = nullptr;
    }
    waterfall_frame_buffers_release();
    const size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_net = psram_after >= psram_before ? psram_after - psram_before : 0U;
    ESP_LOGI(TAG,
        "NSF瀑布隐藏回收：frames=%uB psram_net=%uB reason=%s；音频/Timeline继续",
        static_cast<unsigned>(kWaterfallFrameBytes * 2U),
        static_cast<unsigned>(psram_net),
        reason != nullptr ? reason : "unknown");
}

static void waterfall_schedule_hidden_release()
{
    if (g_waterfall_frame_image == nullptr &&
        g_waterfall_frames[0] == nullptr && g_waterfall_frames[1] == nullptr) {
        waterfall_cancel_hidden_release();
        return;
    }
    g_waterfall_hidden_release_pending = true;
    g_waterfall_hidden_since_tick = static_cast<uint32_t>(lv_tick_get());
    ESP_LOGI(TAG, "NSF瀑布已隐藏：%lums后回收双帧，期间返回播放页直接复用",
        static_cast<unsigned long>(kWaterfallHiddenReleaseDelayMs));
}

static void waterfall_hidden_release_tick()
{
    if (!g_waterfall_hidden_release_pending || g_page != VisualMusicPage::Browser) return;
    const uint32_t now = static_cast<uint32_t>(lv_tick_get());
    if (now - g_waterfall_hidden_since_tick < kWaterfallHiddenReleaseDelayMs) return;
    waterfall_release_double_buffer("browser_hidden_2s");
}

static bool waterfall_enable_double_buffer_if_possible()
{
    waterfall_cancel_hidden_release();
    if (g_waterfall_frame_image != nullptr) return true;
    if (g_waterfall_widget == nullptr) return false;
    if (g_waterfall_frames[0] == nullptr || g_waterfall_frames[1] == nullptr) {
        if (!waterfall_frame_buffers_prepare()) {
            ESP_LOGW(TAG, "NSF瀑布PSRAM双帧申请失败：本次回退LVGL即时绘制");
            return false;
        }
    }
    g_waterfall_frame_image = lv_image_create(g_waterfall_widget);
    if (g_waterfall_frame_image == nullptr) {
        waterfall_frame_buffers_release();
        ESP_LOGW(TAG, "NSF瀑布Image创建失败：本次回退LVGL即时绘制");
        return false;
    }
    ui_common_lock_object(g_waterfall_frame_image);
    lv_image_set_src(g_waterfall_frame_image, &g_waterfall_frame_dsc[0]);
    lv_image_set_antialias(g_waterfall_frame_image, false);
    lv_obj_set_pos(g_waterfall_frame_image, 0, 0);
    ESP_LOGI(TAG, "NSF瀑布PSRAM双帧已准备：2x%uB total=%uB window=-%lums/+%lums events=%u",
        static_cast<unsigned>(kWaterfallFrameBytes),
        static_cast<unsigned>(kWaterfallFrameBytes * 2U),
        static_cast<unsigned long>(kWaterfallNsfPastMs),
        static_cast<unsigned long>(kWaterfallNsfFutureMs),
        static_cast<unsigned>(kNsfVisualWindowCapacity));
    return true;
}

static void waterfall_request_frame(uint32_t now_ms)
{
    if (!waterfall_render_psram_frame(now_ms) && g_waterfall_widget != nullptr) {
        lv_obj_invalidate(g_waterfall_widget);
    }
}

static void waterfall_draw_grid(
    lv_layer_t *layer,
    const lv_area_t &coords)
{
    const int32_t main_x1 = coords.x1 + kWaterfallLowAreaWidth;
    const int32_t main_width = coords.x2 - main_x1 + 1;
    if (main_width <= 0) return;

    lv_draw_rect_dsc_t grid_dsc = {};
    lv_draw_rect_dsc_init(&grid_dsc);
    grid_dsc.radius = 0;
    grid_dsc.border_width = 0;
    grid_dsc.bg_opa = LV_OPA_COVER;

    const int32_t pitch_span = kWaterfallPitchMax - kWaterfallPitchMin;
    for (uint8_t note = kWaterfallPitchMin; note <= kWaterfallPitchMax; ++note) {
        const int32_t x = main_x1 +
            (static_cast<int32_t>(note - kWaterfallPitchMin) * (main_width - 1)) / pitch_span;
        grid_dsc.bg_color = lv_color_hex((note % 12U) == 0U ? 0x263A55 : 0x16263A);
        // 音高线贯通整个控件（含播放线下方历史区），保证音符穿过播放线时背景连续。
        lv_area_t line = {x, coords.y1, x, coords.y2};
        lv_draw_rect(layer, &grid_dsc, &line);
    }

    grid_dsc.bg_color = lv_color_hex(0x31475F);
    lv_area_t divider = {main_x1 - 1, coords.y1, main_x1, coords.y2};
    lv_draw_rect(layer, &grid_dsc, &divider);

    // 左侧两条窄轨统一承载鼓组/Noise/DMC和折叠低音。
    grid_dsc.bg_color = lv_color_hex(0x122033);
    lv_area_t low_split = {coords.x1 + kWaterfallLowAreaWidth / 2, coords.y1,
        coords.x1 + kWaterfallLowAreaWidth / 2, coords.y2};
    lv_draw_rect(layer, &grid_dsc, &low_split);
}

static void waterfall_draw_event(
    lv_layer_t *layer,
    const lv_area_t &coords,
    int32_t current_line_y,
    int32_t future_pixels,
    int32_t past_pixels,
    uint32_t future_ms,
    uint32_t past_ms,
    uint32_t now_ms,
    uint32_t start_ms,
    uint32_t end_ms,
    uint8_t note,
    uint8_t level,
    uint32_t color,
    int8_t low_lane)
{
    if (end_ms < start_ms) return;
    // 播放线就是“现在”：未来窗在其上方随真实时钟向下移动；
    // 已结束/正在演奏的音符落到播放线下方（历史区），让音符完整穿过播放线。
    auto time_to_y = [current_line_y, future_pixels, past_pixels, future_ms, past_ms, now_ms](
        uint32_t time_ms) -> int32_t {
        if (time_ms == now_ms) return current_line_y;
        if (time_ms < now_ms) {
            const uint64_t delta_ms = static_cast<uint64_t>(now_ms) - time_ms;
            if (delta_ms >= past_ms) return current_line_y + past_pixels;
            return current_line_y + static_cast<int32_t>(
                delta_ms * static_cast<uint64_t>(past_pixels) / past_ms);
        }
        const uint64_t delta_ms = static_cast<uint64_t>(time_ms) - now_ms;
        if (delta_ms >= future_ms) return current_line_y - future_pixels;
        return current_line_y - static_cast<int32_t>(
            delta_ms * static_cast<uint64_t>(future_pixels) / future_ms);
    };
    const int32_t y_start = time_to_y(start_ms);
    const int32_t y_end = time_to_y(end_ms);
    int32_t y1 = y_end < y_start ? y_end : y_start;
    int32_t y2 = y_end > y_start ? y_end : y_start;
    if (y2 - y1 < 3) y2 = y1 + 3;
    if (y2 < coords.y1 || y1 > coords.y2) return;
    if (y1 < coords.y1) y1 = coords.y1;
    if (y2 > coords.y2) y2 = coords.y2;

    int32_t center_x = 0;
    int32_t note_width = 6;
    if (low_lane >= 0) {
        const int32_t lane_width = kWaterfallLowAreaWidth / 2;
        center_x = coords.x1 + lane_width * low_lane + lane_width / 2;
        note_width = 9;
    } else {
        const int32_t main_x1 = coords.x1 + kWaterfallLowAreaWidth;
        const int32_t main_width = coords.x2 - main_x1 + 1;
        uint8_t clamped_note = note;
        if (clamped_note < kWaterfallPitchMin) clamped_note = kWaterfallPitchMin;
        if (clamped_note > kWaterfallPitchMax) clamped_note = kWaterfallPitchMax;
        center_x = main_x1 +
            (static_cast<int32_t>(clamped_note - kWaterfallPitchMin) * (main_width - 1)) /
            (kWaterfallPitchMax - kWaterfallPitchMin);
    }

    lv_draw_rect_dsc_t note_dsc = {};
    lv_draw_rect_dsc_init(&note_dsc);
    note_dsc.radius = 0;
    note_dsc.border_width = 0;
    note_dsc.bg_opa = LV_OPA_COVER;

    lv_area_t area = {};
    area.x1 = center_x - note_width / 2;
    area.x2 = area.x1 + note_width - 1;
    if (area.x1 < coords.x1) area.x1 = coords.x1;
    if (area.x2 > coords.x2) area.x2 = coords.x2;

    // 播放线（current_line_y）下方 = 已演奏历史，用同色系固定深色值压暗；
    // 线上 = 未来/当前，保持原亮度。分段绘制，让跨线音符亮度平滑过渡。
    const uint32_t bright_color = waterfall_level_color(color, level);
    const uint32_t dim_color = waterfall_dim_color(color);

    const int32_t bright_y2 = y2 < current_line_y ? y2 : current_line_y;
    if (bright_y2 >= y1) {
        note_dsc.bg_color = lv_color_hex(bright_color);
        area.y1 = y1;
        area.y2 = bright_y2;
        lv_draw_rect(layer, &note_dsc, &area);
    }
    const int32_t dim_y1 = y1 > current_line_y ? y1 : current_line_y;
    if (y2 >= dim_y1) {
        note_dsc.bg_color = lv_color_hex(dim_color);
        area.y1 = dim_y1;
        area.y2 = y2;
        lv_draw_rect(layer, &note_dsc, &area);
    }
}

static void waterfall_draw_cb(lv_event_t *event)
{
    if (g_waterfall_frame_image != nullptr) return;
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN ||
        g_page != VisualMusicPage::NsfReady) {
        return;
    }

    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    if (layer == nullptr || obj == nullptr) return;

    lv_area_t coords = {};
    lv_obj_get_coords(obj, &coords);
    const int32_t width = coords.x2 - coords.x1 + 1;
    const int32_t height = coords.y2 - coords.y1 + 1;
    if (width <= kWaterfallLowAreaWidth + 20 || height <= 20) return;

    const int32_t top = coords.y1 + 4;
    // 播放线上方为未来区，下方留出历史区（当前音符已演奏部分）。
    const int32_t current_line_y = coords.y2 - kWaterfallPastPixels - 2;
    const int32_t future_pixels = current_line_y - top;
    const int32_t past_pixels = coords.y2 - current_line_y;
    if (future_pixels <= 0 || past_pixels <= 0) return;

    // NSF：预读受 Core1 CPU 限制，用更小的窗口保证满窗且平滑。
    uint32_t now_ms = 0U;
    const uint32_t future_ms = kWaterfallNsfFutureMs;
    const uint32_t past_ms = kWaterfallNsfPastMs;
    AudioNsfClockSnapshot clock = {};
    if (audio_service_nsf_get_clock(&clock) && clock.active) {
        now_ms = clock.position_ms > UINT32_MAX
            ? UINT32_MAX
            : static_cast<uint32_t>(clock.position_ms);
    }

    waterfall_draw_grid(layer, coords);

    // 预读事件时间戳 = 歌曲时间轴（与真实播放同步）。直接画全部事件：
    // 未来在上方下落，正在/刚结束的音符完整穿过播放线，已演奏部分留在下方历史区。
    const uint32_t history_start = now_ms > past_ms
        ? now_ms - past_ms
        : 0U;
    const uint32_t future_end = UINT32_MAX - now_ms < future_ms
        ? UINT32_MAX
        : now_ms + future_ms;
    // R6：PCM、当前音符和未来音符全部消费同一个 Sequencer Timeline。
    // 一次复制覆盖历史+未来窗口，避免 UI 对同一份大时间线做两次扫描。
    const size_t count = waterfall_refresh_visual_window(now_ms);
    for (size_t i = 0U; i < count; ++i) {
        const AudioNsfVisualEvent &note = g_nsf_visual_window[i];
        if (note.end_ms < note.start_ms || note.end_ms < history_start ||
            note.start_ms > future_end) continue;
        int8_t low_lane = -1;
        if (note.voice == AudioNsfVisualVoice::Noise) low_lane = 0;
        else if (note.voice == AudioNsfVisualVoice::Dmc ||
                note.note < kWaterfallPitchMin) low_lane = 1;
        waterfall_draw_event(
            layer, coords, current_line_y, future_pixels, past_pixels,
            future_ms, past_ms, now_ms,
            note.start_ms, note.end_ms, note.note, note.level,
            low_lane >= 0 ? 0xF4A62A : nsf_waterfall_color(note.voice), low_lane);
    }

    lv_draw_rect_dsc_t line_dsc = {};
    lv_draw_rect_dsc_init(&line_dsc);
    line_dsc.bg_color = lv_color_hex(0x30D7FF);
    line_dsc.bg_opa = LV_OPA_COVER;
    line_dsc.radius = 0;
    line_dsc.border_width = 0;
    lv_area_t line = {coords.x1 + 2, current_line_y, coords.x2 - 2, current_line_y + 1};
    lv_draw_rect(layer, &line_dsc, &line);
}

static uint32_t nsf_start_current_generation()
{
    uint32_t generation = 0U;
    portENTER_CRITICAL(&g_nsf_start_mux);
    generation = g_nsf_start_generation;
    portEXIT_CRITICAL(&g_nsf_start_mux);
    return generation;
}

static bool nsf_start_task_is_active(uint32_t *generation)
{
    bool active = false;
    uint32_t active_generation = 0U;
    portENTER_CRITICAL(&g_nsf_start_mux);
    active = g_nsf_start_task_active;
    active_generation = g_nsf_start_active_generation;
    portEXIT_CRITICAL(&g_nsf_start_mux);
    if (generation != nullptr) *generation = active_generation;
    return active;
}

static void cancel_nsf_start_request()
{
    portENTER_CRITICAL(&g_nsf_start_mux);
    ++g_nsf_start_generation;
    if (g_nsf_start_generation == 0U) ++g_nsf_start_generation;
    g_nsf_start_result_pending = false;
    g_nsf_start_result_generation = 0U;
    portEXIT_CRITICAL(&g_nsf_start_mux);
    g_nsf_start_deferred = false;
}

static bool take_nsf_start_result(uint32_t *generation, bool *success)
{
    if (generation == nullptr || success == nullptr) return false;
    bool pending = false;
    portENTER_CRITICAL(&g_nsf_start_mux);
    if (g_nsf_start_result_pending) {
        *generation = g_nsf_start_result_generation;
        *success = g_nsf_start_result_success;
        g_nsf_start_result_pending = false;
        pending = true;
    }
    portEXIT_CRITICAL(&g_nsf_start_mux);
    return pending;
}

static void nsf_start_task(void *arg)
{
    NsfStartTaskArgs *args = static_cast<NsfStartTaskArgs *>(arg);
    if (args == nullptr || args->prg == nullptr || args->prg_size == 0U) {
        if (args != nullptr) {
            if (args->prg != nullptr) heap_caps_free(args->prg);
            heap_caps_free(args);
        }
        portENTER_CRITICAL(&g_nsf_start_mux);
        g_nsf_start_task_active = false;
        g_nsf_start_active_generation = 0U;
        portEXIT_CRITICAL(&g_nsf_start_mux);
        vTaskDelete(nullptr);
        return;
    }

    bool success = audio_service_nsf_start(args->prg, args->prg_size, &args->config, true);
    heap_caps_free(args->prg);
    args->prg = nullptr;

    bool current = false;
    portENTER_CRITICAL(&g_nsf_start_mux);
    current = args->generation == g_nsf_start_generation;
    portEXIT_CRITICAL(&g_nsf_start_mux);

    // 失败或请求已被切文件/退出取消时，worker自己把可能的半启动状态彻底收口；
    // 保持 task_active 到 stop 完成，避免下一份NSF抢在旧清理之前启动。
    if (!success || !current) {
        const bool stopped = audio_service_nsf_stop(false, true);
        if (!stopped) {
            ESP_LOGW(TAG, "NSF后台启动清理失败：generation=%lu current=%u success=%u",
                static_cast<unsigned long>(args->generation),
                static_cast<unsigned>(current),
                static_cast<unsigned>(success));
        }
    }

    portENTER_CRITICAL(&g_nsf_start_mux);
    if (g_nsf_start_active_generation == args->generation) {
        g_nsf_start_task_active = false;
        g_nsf_start_active_generation = 0U;
    }
    if (current) {
        g_nsf_start_result_generation = args->generation;
        g_nsf_start_result_success = success;
        g_nsf_start_result_pending = true;
    }
    portEXIT_CRITICAL(&g_nsf_start_mux);

    heap_caps_free(args);
    vTaskDelete(nullptr);
}

static void cancel_nsf_player()
{
    cancel_nsf_start_request();
    g_nsf_load_started_tick = 0U;
    VisualMusicNsf::cancel();
    VisualMusicNsf::release_image(&g_nsf_image);
    g_nsf_track = 0U;
    g_nsf_eof = false;
    g_last_nsf_eof_revision = 0U;
    g_last_nsf_time_label_tick = 0U;
    g_last_waterfall_draw_tick = 0U;
    g_nsf_visual_window_count = 0U;
    g_last_nsf_visual_snapshot_tick = 0U;
    g_nsf_final_high_fps = false;
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
    char duration_text[24] = "--:--";
    uint64_t display_position_ms = position_ms;
    if (clock.duration_ms > 0ULL) {
        const uint64_t duration_seconds = clock.duration_ms / 1000ULL;
        snprintf(duration_text, sizeof(duration_text), "%s%llu:%02llu",
            clock.duration_state == AudioNsfDurationState::Estimated ? "~" : "",
            static_cast<unsigned long long>(duration_seconds / 60ULL),
            static_cast<unsigned long long>(duration_seconds % 60ULL));
        // Estimated 只是提前提示，不能让播放时间按估算值回绕；
        // Final EOF 最多只会因最后一个 PCM 块越过结束点几毫秒，此时夹到最终时长。
        if (clock.eof && clock.duration_state == AudioNsfDurationState::Final &&
            display_position_ms > clock.duration_ms) {
            display_position_ms = clock.duration_ms;
        }
    }
    char text[48] = {};
    snprintf(
        text,
        sizeof(text),
        "%llu:%02llu / %s",
        static_cast<unsigned long long>((display_position_ms / 1000ULL) / 60ULL),
        static_cast<unsigned long long>((display_position_ms / 1000ULL) % 60ULL),
        duration_text);
    lv_label_set_text(g_player_time, text);
}

static void update_nsf_ready_ui()
{
    if (g_page != VisualMusicPage::NsfReady || g_nsf_image.track_count == 0U) return;
    const bool waterfall_ready = g_nsf_image.version == 1U &&
        g_nsf_image.expansion_chips == 0U &&
        (g_nsf_image.pal_ntsc_bits & 0x03U) != 0x01U &&
        !g_nsf_failed;
    set_visible(g_waterfall_widget, waterfall_ready);
    set_visible(g_player_time, true);
    set_visible(g_player_message, !waterfall_ready);
    set_visible(g_player_hint, !waterfall_ready);

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

static bool show_active_nsf_player()
{
    if (!g_nsf_audio_active || g_nsf_image.track_count == 0U) return false;

    AudioNsfClockSnapshot clock = {};
    if (!audio_service_nsf_get_clock(&clock) || !clock.active || clock.failed) return false;

    waterfall_cancel_hidden_release();
    g_page = VisualMusicPage::NsfReady;
    set_visible(g_browser_host, false);
    set_browser_header_visible(false);
    set_visible(g_player_host, true);

    const bool waterfall_double_buffer = waterfall_enable_double_buffer_if_possible();
    g_nsf_paused = clock.paused || clock.eof;
    g_nsf_eof = clock.eof;
    g_nsf_failed = false;
    g_nsf_final_high_fps = clock.duration_state == AudioNsfDurationState::Final;
    g_last_nsf_time_label_tick = 0U;
    g_last_waterfall_draw_tick = 0U;
    g_nsf_visual_window_count = 0U;
    g_last_nsf_visual_snapshot_tick = 0U;
    if (g_timer != nullptr) {
        lv_timer_set_period(
            g_timer,
            g_nsf_final_high_fps ? kNsfFinalTimerPeriodMs : kNsfAnalyzingTimerPeriodMs);
    }
    update_nsf_ready_ui();
    const uint32_t frame_now = clock.position_ms > UINT32_MAX
        ? UINT32_MAX : static_cast<uint32_t>(clock.position_ms);
    waterfall_request_frame(frame_now);
    gesture_router_reset();
    ESP_LOGI(TAG,
        "NSF继续播放页：track=%u/%u position=%lums waterfall=%s",
        static_cast<unsigned>(g_nsf_track + 1U),
        static_cast<unsigned>(g_nsf_image.track_count),
        static_cast<unsigned long>(frame_now),
        waterfall_double_buffer ? "PSRAM双帧" : "LVGL即时绘制");
    return true;
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

    const uint32_t generation = nsf_start_current_generation();
    uint32_t active_generation = 0U;
    if (nsf_start_task_is_active(&active_generation)) {
        if (active_generation == generation) return true;
        g_nsf_start_deferred = true;
        if (g_page == VisualMusicPage::NsfReady) {
            set_player_loading_ui("正在切换 NSF…", "上一首正在安全结束，完成后自动播放");
        }
        return true;
    }

    if (!pause_music_for_nsf_exclusive()) {
        set_player_loading_ui("无法暂停后台Music", "返回列表后可继续浏览文件");
        return false;
    }

    // 只在播放页准备瀑布双帧；若用户已经返回Browser，启动成功后保持后台音频即可。
    if (g_page == VisualMusicPage::NsfReady) {
        (void)waterfall_enable_double_buffer_if_possible();
    }

    NsfStartTaskArgs *args = static_cast<NsfStartTaskArgs *>(heap_caps_calloc(
        1U, sizeof(NsfStartTaskArgs), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    uint8_t *prg_copy = static_cast<uint8_t *>(heap_caps_malloc(
        g_nsf_image.prg_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (args == nullptr || prg_copy == nullptr) {
        if (args != nullptr) heap_caps_free(args);
        if (prg_copy != nullptr) heap_caps_free(prg_copy);
        g_nsf_failed = true;
        if (g_page == VisualMusicPage::NsfReady) update_nsf_ready_ui();
        ESP_LOGE(TAG, "NSF后台启动参数申请失败：prg=%uB",
            static_cast<unsigned>(g_nsf_image.prg_size));
        return false;
    }
    memcpy(prg_copy, g_nsf_image.prg_data, g_nsf_image.prg_size);

    args->prg = prg_copy;
    args->prg_size = g_nsf_image.prg_size;
    args->generation = generation;
    args->config.load_address = g_nsf_image.load_address;
    args->config.init_address = g_nsf_image.init_address;
    args->config.play_address = g_nsf_image.play_address;
    args->config.ntsc_speed_us = g_nsf_image.ntsc_speed_us;
    memcpy(args->config.banks, g_nsf_image.banks, sizeof(args->config.banks));
    args->config.version = g_nsf_image.version;
    args->config.track_count = g_nsf_image.track_count;
    args->config.track = g_nsf_track;
    args->config.pal_ntsc_bits = g_nsf_image.pal_ntsc_bits;
    args->config.expansion_chips = g_nsf_image.expansion_chips;

    portENTER_CRITICAL(&g_nsf_start_mux);
    if (generation != g_nsf_start_generation || g_nsf_start_task_active) {
        portEXIT_CRITICAL(&g_nsf_start_mux);
        heap_caps_free(prg_copy);
        heap_caps_free(args);
        g_nsf_start_deferred = true;
        return true;
    }
    g_nsf_start_task_active = true;
    g_nsf_start_active_generation = generation;
    g_nsf_start_result_pending = false;
    portEXIT_CRITICAL(&g_nsf_start_mux);

    const BaseType_t created = xTaskCreatePinnedToCore(
        nsf_start_task,
        "NsfStartTask",
        kNsfStartTaskStack,
        args,
        kNsfStartTaskPriority,
        nullptr,
        kNsfStartTaskCore);
    if (created != pdPASS) {
        portENTER_CRITICAL(&g_nsf_start_mux);
        if (g_nsf_start_active_generation == generation) {
            g_nsf_start_task_active = false;
            g_nsf_start_active_generation = 0U;
        }
        portEXIT_CRITICAL(&g_nsf_start_mux);
        heap_caps_free(prg_copy);
        heap_caps_free(args);
        g_nsf_failed = true;
        if (g_page == VisualMusicPage::NsfReady) update_nsf_ready_ui();
        ESP_LOGE(TAG, "创建NSF后台启动任务失败：track=%u/%u",
            static_cast<unsigned>(g_nsf_track + 1U),
            static_cast<unsigned>(g_nsf_image.track_count));
        return false;
    }

    g_nsf_paused = true;
    g_nsf_eof = false;
    g_nsf_failed = false;
    g_nsf_start_deferred = false;
    if (g_page == VisualMusicPage::NsfReady) {
        set_player_loading_ui("正在初始化 2A03…", "初始化期间可右滑或点击列表返回");
    }
    ESP_LOGI(TAG, "NSF 2A03后台启动已排队：track=%u/%u generation=%lu",
        static_cast<unsigned>(g_nsf_track + 1U),
        static_cast<unsigned>(g_nsf_image.track_count),
        static_cast<unsigned long>(generation));
    return true;
}

static void nsf_start_result_tick()
{
    uint32_t result_generation = 0U;
    bool success = false;
    if (take_nsf_start_result(&result_generation, &success)) {
        if (result_generation != nsf_start_current_generation()) return;

        if (!success) {
            g_nsf_audio_active = false;
            remote_context_end();
            g_nsf_paused = true;
            g_nsf_eof = false;
            g_nsf_failed = true;
            g_nsf_final_high_fps = false;
            if (g_page == VisualMusicPage::NsfReady) update_nsf_ready_ui();
            ESP_LOGE(TAG, "NSF 2A03后台启动失败：track=%u/%u",
                static_cast<unsigned>(g_nsf_track + 1U),
                static_cast<unsigned>(g_nsf_image.track_count));
            return;
        }

        g_nsf_audio_active = true;
        g_nsf_paused = false;
        remote_context_begin();
        g_nsf_eof = false;
        g_last_nsf_eof_revision = 0U;
        g_nsf_failed = false;
        g_last_nsf_time_label_tick = 0U;
        g_last_waterfall_draw_tick = 0U;
        g_nsf_visual_window_count = 0U;
        g_last_nsf_visual_snapshot_tick = 0U;
        g_nsf_final_high_fps = false;
        if (g_timer != nullptr) {
            lv_timer_set_period(
                g_timer,
                g_page == VisualMusicPage::NsfReady
                    ? kNsfAnalyzingTimerPeriodMs
                    : kBrowserTimerPeriodMs);
        }
        if (g_page == VisualMusicPage::NsfReady) {
            update_nsf_ready_ui();
            waterfall_request_frame(0U);
        }
        ESP_LOGI(TAG, "NSF 2A03音频启动：track=%u/%u 48000Hz 基础5通道 waterfall=%s analyzing=8fps final=%ufps",
            static_cast<unsigned>(g_nsf_track + 1U),
            static_cast<unsigned>(g_nsf_image.track_count),
            g_waterfall_frame_image != nullptr ? "PSRAM双帧" : "LVGL即时绘制",
            static_cast<unsigned>(g_waterfall_frame_image != nullptr ? 25U : 20U));
        return;
    }

    // 快速切文件时旧worker必须先彻底stop，之后自动启动当前已经解析好的NSF。
    if (g_nsf_start_deferred && !nsf_start_task_is_active(nullptr) &&
        g_nsf_image.prg_data != nullptr && g_nsf_image.track_count > 0U) {
        g_nsf_start_deferred = false;
        (void)start_nsf_audio_from_image();
    }
}

static void begin_nsf_load()
{
    waterfall_cancel_hidden_release();
    if (g_selected_path == nullptr || g_selected_path[0] == '\0') return;
    // 切换到新的 NSF 文件时只停止旧 NSF；普通 Music 始终保持进入 NSF 前的暂停状态。
    // 只有真正离开电子音流 APP 时，visual_music_leave() 才恢复普通 Music。
    (void)stop_nsf_audio(false, "switch_nsf_file");
    cancel_nsf_player();
    g_nsf_paused = true;
    g_nsf_eof = false;
    g_last_nsf_eof_revision = 0U;
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
        g_nsf_load_started_tick = 0U;
        set_player_loading_ui("NSF解析任务启动失败", esp_err_to_name(ret));
        ESP_LOGW(TAG, "NSF解析任务启动失败：path=%s ret=%s", g_selected_path, esp_err_to_name(ret));
    } else {
        g_nsf_load_started_tick = static_cast<uint32_t>(lv_tick_get());
        ESP_LOGI(TAG, "NSF解析已排队：%s", g_selected_path);
    }
    gesture_router_reset();
}

static void nsf_result_tick()
{
    if (g_page != VisualMusicPage::NsfLoading) return;
    VisualMusicNsf::LoadResult result = {};
    if (!VisualMusicNsf::take_result(&result)) {
        if (g_nsf_load_started_tick != 0U) {
            const uint32_t now = static_cast<uint32_t>(lv_tick_get());
            if (now - g_nsf_load_started_tick >= kNsfLoadTimeoutMs) {
                VisualMusicNsf::cancel();
                g_nsf_load_started_tick = 0U;
                set_player_loading_ui("NSF解析超时", "可右滑或点击列表返回后重试");
                ESP_LOGE(TAG, "NSF解析超时：>%lums path=%s",
                    static_cast<unsigned long>(kNsfLoadTimeoutMs),
                    g_selected_path != nullptr ? g_selected_path : "(null)");
            }
        }
        return;
    }
    g_nsf_load_started_tick = 0U;
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
    g_last_nsf_eof_revision = 0U;
    g_nsf_failed = false;
    g_page = VisualMusicPage::NsfReady;
    update_nsf_ready_ui();
    gesture_router_reset();
    ESP_LOGI(
        TAG,
        "NSF Subsong已就绪：track=%u/%u expansion=0x%02X timing=0x%02X",
        static_cast<unsigned>(g_nsf_track + 1U),
        static_cast<unsigned>(g_nsf_image.track_count),
        static_cast<unsigned>(g_nsf_image.expansion_chips),
        static_cast<unsigned>(g_nsf_image.pal_ntsc_bits & 0x03U));

    if (g_nsf_image.version == 1U && g_nsf_image.expansion_chips == 0U &&
        (g_nsf_image.pal_ntsc_bits & 0x03U) != 0x01U) {
        (void)start_nsf_audio_from_image();
    } else {
        ESP_LOGW(TAG, "NSF已解析但不自动启动：version=%u expansion=0x%02X timing=0x%02X",
            static_cast<unsigned>(g_nsf_image.version),
            static_cast<unsigned>(g_nsf_image.expansion_chips),
            static_cast<unsigned>(g_nsf_image.pal_ntsc_bits & 0x03U));
    }
}

static bool select_nsf_subsong(int direction, bool allow_wrap)
{
    // NSF 会话可以在文件列表页继续播放，内部 Track 切换不能依赖当前 UI 页面。
    if (g_nsf_image.track_count == 0U || direction == 0) {
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
        // EOF 自动切歌必须区分“没有下一首”和“AudioTask 切换失败”。
        // 后者允许下一次 timer 重新消费 EOF，避免一次瞬态失败后永久卡在曲尾。
        if (restarting_from_eof) g_last_nsf_eof_revision = 0U;
        ESP_LOGE(TAG, "NSF Subsong切换失败：track=%u/%u%s",
            static_cast<unsigned>(next_track + 1U),
            static_cast<unsigned>(g_nsf_image.track_count),
            restarting_from_eof ? "；保留EOF供重试" : "");
        return false;
    }
    g_nsf_track = next_track;
    remote_identity_changed();
    if (restarting_from_eof) g_nsf_paused = false;
    g_nsf_eof = false;
    g_nsf_failed = false;
    g_last_nsf_time_label_tick = 0U;
    g_last_waterfall_draw_tick = 0U;
    g_nsf_visual_window_count = 0U;
    g_last_nsf_visual_snapshot_tick = 0U;
    g_nsf_final_high_fps = false;
    if (g_timer != nullptr) {
        lv_timer_set_period(
            g_timer,
            g_page == VisualMusicPage::NsfReady
                ? kNsfAnalyzingTimerPeriodMs
                : kBrowserTimerPeriodMs);
    }
    update_nsf_ready_ui();
    if (g_page == VisualMusicPage::NsfReady) waterfall_request_frame(0U);
    ESP_LOGI(
        TAG,
        "NSF Subsong选择：track=%u/%u",
        static_cast<unsigned>(g_nsf_track + 1U),
        static_cast<unsigned>(g_nsf_image.track_count));
    return true;
}

static void show_browser();

static bool toggle_nsf_playback()
{
    if (!g_nsf_audio_active) {
        if (g_page != VisualMusicPage::NsfReady) return false;
        return start_nsf_audio_from_image();
    }

    AudioNsfClockSnapshot clock = {};
    if (!audio_service_nsf_get_clock(&clock) || !clock.active || clock.failed) return false;
    bool success = false;
    if (clock.eof) {
        success = audio_service_nsf_set_track(g_nsf_track, true);
        if (success) {
            g_nsf_paused = false;
            g_nsf_eof = false;
        }
    } else if (clock.paused) {
        success = audio_service_nsf_resume(true);
        if (success) g_nsf_paused = false;
    } else {
        success = audio_service_nsf_pause(true);
        if (success) g_nsf_paused = true;
    }
    if (!success) return false;
    remote_playback_changed();
    if (g_page == VisualMusicPage::NsfReady) {
        update_nsf_time_label();
        update_player_controls();
    }
    return true;
}

static bool select_playable_index(size_t index)
{
    const VisualMusicBrowser::EntryIndex *entry = VisualMusicBrowser::entry_at(&g_directory, index);
    const char *name = VisualMusicBrowser::entry_name(&g_directory, index);
    if (entry == nullptr || name == nullptr || VisualMusicBrowser::entry_is_directory(entry) ||
        !VisualMusicBrowser::entry_is_nsf(entry) ||
        VisualMusicBrowser::join_child_path(
            g_current_dir,
            name,
            g_selected_path,
            VisualMusicBrowser::kPathBytes) != ESP_OK) {
        return false;
    }

    g_selected_index = index;
    begin_nsf_load();
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
    if (g_page == VisualMusicPage::NsfReady) (void)toggle_nsf_playback();
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
        show_status("/synth 中没有 NSF", 0x7F8A99);
        return;
    }
    if (g_browser_status != nullptr) set_visible(g_browser_status, false);

    const size_t last_page_first =
        ((g_directory.count - 1U) / kVisibleRows) * kVisibleRows;
    if (g_first_index > last_page_first) g_first_index = last_page_first;

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
        lv_label_set_text(ui.name, name);
        lv_label_set_text(ui.kind, VisualMusicBrowser::entry_kind_name(entry));
        lv_obj_set_style_text_color(
            ui.name,
            lv_color_hex(is_dir ? 0xBCD7FF : 0xEEF1F5),
            0);
        lv_obj_set_style_text_color(
            ui.kind,
            lv_color_hex(is_dir ? 0x6EA5F2 : 0xC79BFF),
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
    const size_t last_page_first =
        ((g_directory.count - 1U) / kVisibleRows) * kVisibleRows;
    if (direction > 0) {
        const size_t candidate = old + kVisibleRows;
        g_first_index = candidate < last_page_first ? candidate : last_page_first;
    } else {
        g_first_index = old >= kVisibleRows ? old - kVisibleRows : 0U;
    }
    if (old == g_first_index) return;
    update_rows();
    ESP_LOGI(
        TAG,
        "电子音流目录分页：first=%u total=%u direction=%s",
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
        g_current_dir == nullptr || app_manager_foreground() != AppId::Nsf ||
        app_launcher_overlay_is_visible()) {
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
    // 播放页 -> 文件列表只切换 UI：NSF 继续播放，普通 Music 继续保持暂停。
    // 若用户在“正在解析新 NSF”阶段返回列表，则仅取消尚未开始的解析。
    if (g_page == VisualMusicPage::NsfLoading) {
        cancel_nsf_player();
    }
    g_page = VisualMusicPage::Browser;
    if (g_timer != nullptr) lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
    set_visible(g_browser_host, true);
    set_browser_header_visible(true);
    set_visible(g_player_host, false);
    if (g_nsf_audio_active) waterfall_schedule_hidden_release();
    else waterfall_release_double_buffer("browser_no_active_nsf");
    update_header();
    update_rows();
    gesture_router_reset();
    ESP_LOGI(
        TAG,
        "电子音流返回文件列表：NSF=%s Music=%s%s；离开APP后才恢复Music",
        g_nsf_audio_active ? "继续播放" : "未播放",
        (g_music_paused_for_nsf || g_music_deep_suspended_for_nsf) ? "保持暂停" : "保持原状态",
        g_music_deep_suspended_for_nsf ? "/DeepSuspend" : "");
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

    const bool reopen_active_nsf = g_nsf_audio_active && g_nsf_image.track_count > 0U &&
        g_selected_path != nullptr && strcmp(g_selected_path, g_scratch_path) == 0;
    g_selected_index = index;
    if (reopen_active_nsf && show_active_nsf_player()) return;
    snprintf(g_selected_path, VisualMusicBrowser::kPathBytes, "%s", g_scratch_path);
    begin_nsf_load();
}

static void back_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click()) {
        return;
    }
    go_back();
}

static void process_remote_command()
{
    NsfRemoteCommand command = NsfRemoteCommand::TogglePlayPause;
    if (!remote_take_pending(&command)) return;

    bool ok = false;
    if (command == NsfRemoteCommand::TogglePlayPause) {
        ok = toggle_nsf_playback();
    } else {
        ok = select_nsf_subsong(
            command == NsfRemoteCommand::Next ? +1 : -1,
            g_loop_mode == PlayerLoopMode::RepeatAll);
    }

    if (ok) {
        ESP_LOGI(TAG, "BLE NSF控制：%s track=%u/%u",
            command == NsfRemoteCommand::TogglePlayPause
                ? (g_nsf_paused ? "暂停" : "继续播放")
                : (command == NsfRemoteCommand::Next ? "下一Track" : "上一Track"),
            static_cast<unsigned>(g_nsf_track + 1U),
            static_cast<unsigned>(g_nsf_image.track_count));
    } else {
        ESP_LOGW(TAG, "BLE NSF控制失败：command=%u track=%u/%u",
            static_cast<unsigned>(command),
            static_cast<unsigned>(g_nsf_track + 1U),
            static_cast<unsigned>(g_nsf_image.track_count));
    }
}

static void timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_root == nullptr || app_manager_foreground() != AppId::Nsf) return;

    process_remote_command();
    nsf_result_tick();
    nsf_start_result_tick();
    if (g_page == VisualMusicPage::Browser) browser_load_tick();
    waterfall_hidden_release_tick();
    // NSF 音频会话与页面显示解耦：Browser 中仍持续消费 Clock/EOF/循环状态。
    if (g_nsf_audio_active) {
        const bool player_visible = g_page == VisualMusicPage::NsfReady;
        const uint32_t now_tick = static_cast<uint32_t>(lv_tick_get());
        AudioNsfClockSnapshot clock = {};
        if (audio_service_nsf_get_clock(&clock) && clock.active) {
            const bool paused_changed = g_nsf_paused != (clock.paused || clock.eof);
            g_nsf_paused = clock.paused || clock.eof;
            g_nsf_eof = clock.eof;
            g_nsf_failed = clock.failed;
            if (clock.track < g_nsf_image.track_count) g_nsf_track = clock.track;
            if (clock.failed) {
                // 运行失败也不越过 APP 生命周期恢复普通 Music。
                (void)stop_nsf_audio(false, "nsf_runtime_failed");
                g_nsf_failed = true;
                if (player_visible) update_nsf_ready_ui();
            } else if (clock.eof) {
                // NSF v1 没有原生 Track 时长：AudioTask 只在 Final 结束计划真正到点后发布 EOF。
                // UI 按 Track 实例 revision 只消费一次 EOF，并按当前循环模式切换 Subsong。
                if (clock.revision != 0U && clock.revision != g_last_nsf_eof_revision) {
                    g_last_nsf_eof_revision = clock.revision;
                    ESP_LOGI(TAG, "NSF EOF消费：track=%u/%u revision=%lu mode=%s",
                        static_cast<unsigned>(g_nsf_track + 1U),
                        static_cast<unsigned>(g_nsf_image.track_count),
                        static_cast<unsigned long>(clock.revision),
                        loop_mode_text());
                    if (g_loop_mode == PlayerLoopMode::RepeatOne) {
                        // EOF 后重启必须等待 AudioTask 真正完成 reset。异步提交只代表“已入队”，
                        // 底层 mute/INIT/reset 失败时会让 UI 误以为 EOF 已消费并永久卡住。
                        if (audio_service_nsf_set_track(g_nsf_track, true)) {
                            g_nsf_paused = false;
                            g_nsf_eof = false;
                            remote_playback_changed();
                            g_last_nsf_time_label_tick = 0U;
                            g_last_waterfall_draw_tick = 0U;
                            if (player_visible) waterfall_request_frame(0U);
                            ESP_LOGI(TAG, "NSF RepeatOne已重启：track=%u/%u",
                                static_cast<unsigned>(g_nsf_track + 1U),
                                static_cast<unsigned>(g_nsf_image.track_count));
                        } else {
                            g_last_nsf_eof_revision = 0U;
                            ESP_LOGW(TAG, "NSF RepeatOne重启失败：track=%u/%u；保留EOF供重试",
                                static_cast<unsigned>(g_nsf_track + 1U),
                                static_cast<unsigned>(g_nsf_image.track_count));
                        }
                    } else if (select_nsf_subsong(
                                   +1,
                                   g_loop_mode == PlayerLoopMode::RepeatAll)) {
                        return;
                    }
                    if (player_visible) {
                        update_nsf_time_label();
                        update_player_controls();
                    }
                }
            } else {
                if (paused_changed) {
                    if (player_visible) update_player_controls();
                    if (player_visible && clock.paused) {
                        // Pause 只在状态切换瞬间补一帧，之后不再持续刷新瀑布。
                        const uint32_t frame_now = clock.position_ms > UINT32_MAX
                            ? UINT32_MAX : static_cast<uint32_t>(clock.position_ms);
                        waterfall_request_frame(frame_now);
                    }
                }
                const bool want_final_high_fps =
                    clock.duration_state == AudioNsfDurationState::Final;
                if (want_final_high_fps != g_nsf_final_high_fps) {
                    g_nsf_final_high_fps = want_final_high_fps;
                    if (g_timer != nullptr) {
                        lv_timer_set_period(
                            g_timer,
                            player_visible
                                ? (want_final_high_fps
                                    ? kNsfFinalTimerPeriodMs
                                    : kNsfAnalyzingTimerPeriodMs)
                                : kBrowserTimerPeriodMs);
                    }
                    g_last_waterfall_draw_tick = 0U;
                    if (player_visible && want_final_high_fps) {
                        ESP_LOGI(TAG, "NSF时长Final：瀑布切换%ufps（%s）",
                            static_cast<unsigned>(g_waterfall_frame_image != nullptr ? 25U : 20U),
                            g_waterfall_frame_image != nullptr ? "PSRAM双帧" : "LVGL即时绘制");
                    }
                }
                if (player_visible &&
                    (g_last_nsf_time_label_tick == 0U ||
                     now_tick - g_last_nsf_time_label_tick >= kWaterfallTimeLabelPeriodMs)) {
                    g_last_nsf_time_label_tick = now_tick;
                    update_nsf_time_label();
                }
                if (player_visible && !clock.paused) {
                    const bool waterfall_double_buffer =
                        g_waterfall_frame_image != nullptr;
                    const uint32_t waterfall_period_ms =
                        clock.duration_state == AudioNsfDurationState::Final
                            ? (waterfall_double_buffer
                                ? kNsfWaterfallDoubleBufferFinalFramePeriodMs
                                : kNsfWaterfallFinalFramePeriodMs)
                            : kNsfWaterfallAnalyzingFramePeriodMs;
                    if (g_last_waterfall_draw_tick == 0U ||
                        now_tick - g_last_waterfall_draw_tick >= waterfall_period_ms) {
                        g_last_waterfall_draw_tick = now_tick;
                        const uint32_t frame_now = clock.position_ms > UINT32_MAX
                            ? UINT32_MAX : static_cast<uint32_t>(clock.position_ms);
                        waterfall_request_frame(frame_now);
                    }
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
    g_waterfall_frame_image = nullptr;
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
    if (g_nsf_visual_window != nullptr) heap_caps_free(g_nsf_visual_window);
    waterfall_frame_buffers_release();
    g_current_dir = nullptr;
    g_scratch_path = nullptr;
    g_selected_path = nullptr;
    g_nsf_visual_window = nullptr;
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
    g_nsf_visual_window = static_cast<AudioNsfVisualEvent *>(heap_caps_calloc(
        kNsfVisualWindowCapacity,
        sizeof(AudioNsfVisualEvent),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_current_dir == nullptr || g_scratch_path == nullptr || g_selected_path == nullptr ||
        g_nsf_visual_window == nullptr) {
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
    lv_obj_set_style_bg_color(g_waterfall_widget, lv_color_hex(0x071425), 0);
    lv_obj_set_style_bg_opa(g_waterfall_widget, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_waterfall_widget, 0, 0);
    lv_obj_remove_flag(g_waterfall_widget, LV_OBJ_FLAG_SCROLLABLE);
    // 双帧只在真正启动NSF后申请：浏览文件列表时不额外常驻约557KB PSRAM。
    // 回调始终保留，双帧启用后会在入口快速返回；PSRAM不足时自然回退原即时绘制。
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
    lv_obj_set_pos(g_player_controls, 0, 460 - kPlayerControlHeight);
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

    ESP_LOGI(TAG, "电子音流 create完成：5-Row Browser + 三段式NSF Waterfall Player");
    return ESP_OK;
}

static esp_err_t visual_music_enter()
{
    if (g_root == nullptr || g_current_dir == nullptr) return ESP_ERR_INVALID_STATE;

    g_nsf_audio_active = false;
    g_nsf_paused = true;
    g_nsf_failed = false;
    g_music_paused_for_nsf = false;
    g_music_deep_suspended_for_nsf = false;
    waterfall_cancel_hidden_release();
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
        "电子音流进入Foreground：/synth 浏览器已显示；NSF使用6502/2A03基础5通道");
    return ESP_OK;
}

static esp_err_t visual_music_leave(AppRunState next_state)
{
    (void)next_state;
    cancel_browser_load();
    if (!stop_nsf_audio(true, "app_leave")) return ESP_ERR_INVALID_STATE;
    waterfall_release_double_buffer("app_leave");
    cancel_nsf_player();
    if (g_timer != nullptr) lv_timer_pause(g_timer);
    gesture_router_reset();
    app_launcher_overlay_hide();
    set_visible(g_root, false);
    g_page = VisualMusicPage::Browser;
    ESP_LOGI(TAG, "电子音流离开Foreground：NSF音频与目录协作任务已停止；此时才恢复进入前Music状态");
    return ESP_OK;
}

static void visual_music_destroy()
{
    waterfall_cancel_hidden_release();
    cancel_browser_load();
    (void)stop_nsf_audio(true, "app_destroy");
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
    g_waterfall_frame_image = nullptr;
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
    if (g_nsf_visual_window != nullptr) heap_caps_free(g_nsf_visual_window);
    waterfall_frame_buffers_release();
    g_current_dir = nullptr;
    g_scratch_path = nullptr;
    g_selected_path = nullptr;
    g_nsf_visual_window = nullptr;
    g_first_index = 0U;
    g_selected_index = SIZE_MAX;
    g_loop_mode = PlayerLoopMode::Sequential;
    g_nsf_audio_active = false;
    g_nsf_paused = true;
    g_nsf_failed = false;
    g_music_paused_for_nsf = false;
    g_music_deep_suspended_for_nsf = false;
    g_page = VisualMusicPage::Browser;
    ESP_LOGI(TAG, "电子音流 destroy完成：LVGL/目录索引/路径/瀑布双帧PSRAM已释放");
}

} // namespace

bool visual_music_app_remote_control_active()
{
    bool active = false;
    portENTER_CRITICAL(&g_remote_mux);
    active = g_remote_context_active;
    portEXIT_CRITICAL(&g_remote_mux);
    return active;
}

bool visual_music_app_remote_submit(NsfRemoteCommand command)
{
    bool accepted = false;
    portENTER_CRITICAL(&g_remote_mux);
    if (g_remote_context_active && !g_remote_pending_valid) {
        g_remote_pending = command;
        g_remote_pending_valid = true;
        accepted = true;
    }
    portEXIT_CRITICAL(&g_remote_mux);
    return accepted;
}

bool visual_music_app_get_remote_snapshot(NsfRemoteSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;

    NsfRemoteSnapshot snapshot = {};
    portENTER_CRITICAL(&g_remote_mux);
    snapshot.active = g_remote_context_active;
    snapshot.track_index = g_remote_track_index;
    snapshot.playback_revision = g_remote_playback_revision;
    snapshot.metadata_revision = g_remote_metadata_revision;
    memcpy(snapshot.title, g_remote_title, sizeof(snapshot.title));
    memcpy(snapshot.artist, g_remote_artist, sizeof(snapshot.artist));
    portEXIT_CRITICAL(&g_remote_mux);

    if (!snapshot.active) {
        *out_snapshot = snapshot;
        return true;
    }

    AudioNsfClockSnapshot clock = {};
    if (audio_service_nsf_get_clock(&clock) && clock.active && !clock.failed) {
        snapshot.track_index = clock.track;
        snapshot.position_ms = clock.position_ms > UINT32_MAX
            ? UINT32_MAX : static_cast<uint32_t>(clock.position_ms);
        snapshot.duration_ms = clock.duration_ms > UINT32_MAX
            ? UINT32_MAX : static_cast<uint32_t>(clock.duration_ms);
        snapshot.paused = clock.paused || clock.eof;
        snapshot.playing = !snapshot.paused;
    } else {
        // Clock切换瞬间暂不可读时保持NSF上下文，但对手机呈现Preparing而不是误报Paused。
        snapshot.paused = false;
        snapshot.playing = false;
    }

    *out_snapshot = snapshot;
    return true;
}

esp_err_t visual_music_app_register()
{
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
        ESP_LOGI(TAG, "电子音流 APP已注册：NSF 6502/2A03 + 统一AppManager生命周期");
    }
    return ret;
}
