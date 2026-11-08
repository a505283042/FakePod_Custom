#include "video_app.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_launcher_overlay.h"
#include "app_manager.h"
#include "audio/decoders/flac_decoder.h"
#include "audio/audio_service.h"
#include "drivers/display/display_bounded_spi.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "font/font_manager.h"
#include "gesture/gesture_router.h"
#include "lvgl.h"
#include "media_library.h"
#include "player_state.h"
#include "player_transport.h"
#include "device_settings.h"
#include "screen_lock_simple.h"
#include "ui_common.h"
#include "video_benchmark.h"
#include "video_browser_model.h"
#include "video_probe.h"

static const char *TAG = "VideoAPP";

namespace
{

enum class VideoPage : uint8_t
{
    Browser = 0,
    Benchmark,
};

enum class BrowserLoadPhase : uint8_t
{
    Idle = 0,
    WaitingForAudioWindow,
    Scanning,
};

enum class EofTransition : uint8_t
{
    None = 0,
    AutoNext,
    ReturnBrowser,
};

static constexpr int32_t kHeaderHeight = 68;
static constexpr int32_t kContentMargin = 12;
static constexpr int32_t kRowHeight = 58;
static constexpr int32_t kRowGap = 8;
static constexpr size_t kVisibleRows = 5U;
static constexpr uint32_t kFlacSafePercent = 90U;
static constexpr size_t kScanBatchNoFlac = 8U;
static constexpr size_t kScanBatchWithFlac = 1U;
static constexpr uint32_t kWaitLogIntervalMs = 1000U;
static constexpr int64_t kVideoLateDropUs = 50000LL;
// 音频主时钟看门狗：音频欠载时“已提交 PCM 时钟”会停滞，累计落后视频 PTS 超过该值后，
// 本段改用墙钟节拍，避免音频缺料把整段画面拖成慢放。
static constexpr int64_t kAudioMasterMaxLagUs = 200000LL;
// 59.8Hz TE 半周期约 8.4ms；Dedicated Presenter 在 RGB ready queue 唤醒后直接做 PTS pacing。
// 本轮保持 R.40.3.3.2 的 14ms TE arm 不变，只隔离“LVGL轮询”这一变量。
static constexpr int64_t kBenchmarkTeArmLeadUs = 14000LL;
// Minimal Tear Guard：460x460 实机在 Y=0、仅 300us TE 后微相位时已无撕裂。
// R.40.3.3.3.2 不再按 frame_y 追加 2~4ms 扫描等待，所有尺寸统一只保留 300us。
// 仅改 TE 后 stream 微相位，不改 PTS/Presenter/Decode 行为。
static constexpr uint32_t kScanPhaseSafetyUs = 300U;
static constexpr uint32_t kDestroyCleanupWaitMs = 300U;
static constexpr uint32_t kBrowserTimerPeriodMs = 20U;
static constexpr uint32_t kBenchmarkTimerPeriodMs = 5U;
static constexpr uint32_t kPresenterTaskStack = 4096U;
// 视频播放双核调度：Core0 保留 AudioTask=P5，Presenter=P2 与 Extractor=P2 同级分享时间片。
// Presenter 大量时间在PTS/TE/SPI等待，不再用P3抢占Extractor；Core1 仅保留 JPEG Decode=P2 持续CPU计算。
static constexpr UBaseType_t kPresenterTaskPriority = 2U;
static constexpr BaseType_t kPresenterTaskCore = 0;
static constexpr uint32_t kPresenterFrameWaitMs = 20U;
static constexpr uint32_t kPresenterStopWaitMs = 250U;
// R.40.5 实机验证阶段：设置页接入前临时开启，便于验证自然结束连续播放。
// 下一阶段由“自动播放下一个视频”设置项接管，不在视频画面增加浮动UI。
static constexpr bool kAutoPlayNextBringUpEnabled = true;
static constexpr size_t kInvalidEntryIndex = static_cast<size_t>(-1);

static constexpr int64_t kVideoControlsOverlayHoldUs = 3000000LL;
static constexpr int32_t kVideoControlsDragFullScalePx = 220;

enum class VideoControlTarget : uint8_t { None = 0, Brightness, Volume };

struct VideoControlsLayout
{
    bool valid = false;
    uint16_t brightness_left_local = 0U;
    uint16_t volume_left_local = 0U;
    uint16_t pill_top_local = 0U;
    uint16_t pill_width = 0U;
    uint16_t pill_height = 0U;
    uint16_t panel_left_local = 0U;
    uint16_t panel_top_local = 0U;
    uint16_t panel_width = 0U;
    uint16_t panel_height = 0U;
    int16_t brightness_left_screen = 0;
    int16_t brightness_top_screen = 0;
    int16_t brightness_right_screen = 0;
    int16_t brightness_bottom_screen = 0;
    int16_t volume_left_screen = 0;
    int16_t volume_top_screen = 0;
    int16_t volume_right_screen = 0;
    int16_t volume_bottom_screen = 0;
    int16_t panel_left_screen = 0;
    int16_t panel_top_screen = 0;
    int16_t panel_right_screen = 0;
    int16_t panel_bottom_screen = 0;
};

struct VideoControlsState
{
    bool active = false;
    VideoControlTarget selected = VideoControlTarget::None;
    int64_t until_us = 0LL;
    uint8_t brightness_percent = 60U;
    uint8_t volume_percent = 50U;
    bool brightness_dirty = false;
    bool dragging = false;
    uint32_t drag_sequence = 0U;
    uint8_t drag_start_percent = 0U;
    VideoControlsLayout layout = {};
};

struct VideoControlsDrawSnapshot
{
    bool active = false;
    VideoControlTarget selected = VideoControlTarget::None;
    uint8_t brightness_percent = 60U;
    uint8_t volume_percent = 50U;
    uint8_t slider_percent = 0U;
    VideoControlsLayout layout = {};
};

static VideoControlsState g_video_controls = {};
static portMUX_TYPE g_video_controls_mux = portMUX_INITIALIZER_UNLOCKED;

static void benchmark_controls_hide()
{
    portENTER_CRITICAL(&g_video_controls_mux);
    g_video_controls.active = false;
    g_video_controls.selected = VideoControlTarget::None;
    g_video_controls.until_us = 0LL;
    g_video_controls.dragging = false;
    g_video_controls.layout.valid = false;
    portEXIT_CRITICAL(&g_video_controls_mux);
    gesture_router_set_vertical_adjust_enabled(false);
    gesture_router_set_vertical_adjust_edges_reserved(false);
    gesture_router_set_control_capture(false);
}

static void benchmark_controls_show()
{
    portENTER_CRITICAL(&g_video_controls_mux);
    g_video_controls.active = true;
    g_video_controls.selected = VideoControlTarget::None;
    g_video_controls.until_us = esp_timer_get_time() + kVideoControlsOverlayHoldUs;
    g_video_controls.dragging = false;
    g_video_controls.layout.valid = false;
    portEXIT_CRITICAL(&g_video_controls_mux);
    gesture_router_set_vertical_adjust_enabled(false);
    gesture_router_set_vertical_adjust_edges_reserved(false);
}

static void benchmark_controls_select(VideoControlTarget target)
{
    portENTER_CRITICAL(&g_video_controls_mux);
    g_video_controls.active = true;
    g_video_controls.selected = target;
    g_video_controls.until_us = esp_timer_get_time() + kVideoControlsOverlayHoldUs;
    g_video_controls.dragging = false;
    g_video_controls.layout.valid = false;
    portEXIT_CRITICAL(&g_video_controls_mux);
    const bool adjust_enabled = target != VideoControlTarget::None;
    gesture_router_set_vertical_adjust_enabled(adjust_enabled);
    gesture_router_set_vertical_adjust_edges_reserved(adjust_enabled);
}

static void benchmark_controls_extend()
{
    portENTER_CRITICAL(&g_video_controls_mux);
    if (g_video_controls.active) {
        g_video_controls.until_us = esp_timer_get_time() + kVideoControlsOverlayHoldUs;
    }
    portEXIT_CRITICAL(&g_video_controls_mux);
}

static void benchmark_controls_reset_for_video()
{
    uint8_t volume = 50U;
    (void)audio_service_video_mp3_get_volume(&volume);
    const uint8_t brightness = screen_lock_simple_get_normal_brightness();
    portENTER_CRITICAL(&g_video_controls_mux);
    g_video_controls.active = false;
    g_video_controls.selected = VideoControlTarget::None;
    g_video_controls.until_us = 0LL;
    g_video_controls.brightness_percent = brightness < 5U ? 5U : brightness;
    g_video_controls.volume_percent = volume > 100U ? 100U : volume;
    g_video_controls.dragging = false;
    g_video_controls.layout.valid = false;
    portEXIT_CRITICAL(&g_video_controls_mux);
    gesture_router_set_vertical_adjust_enabled(false);
    gesture_router_set_vertical_adjust_edges_reserved(false);
    gesture_router_set_single_tap_enabled(true);
}

static void benchmark_controls_commit_brightness()
{
    uint8_t level = 60U;
    bool dirty = false;
    portENTER_CRITICAL(&g_video_controls_mux);
    level = g_video_controls.brightness_percent;
    dirty = g_video_controls.brightness_dirty;
    portEXIT_CRITICAL(&g_video_controls_mux);
    if (!dirty) return;
    const esp_err_t ret = device_settings_set_brightness_level(level);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&g_video_controls_mux);
        if (g_video_controls.brightness_percent == level) g_video_controls.brightness_dirty = false;
        portEXIT_CRITICAL(&g_video_controls_mux);
        ESP_LOGI(TAG, "视频亮度已保存：%u%%", static_cast<unsigned>(level));
    } else {
        ESP_LOGW(TAG, "视频亮度保存失败：level=%u ret=%s",
            static_cast<unsigned>(level), esp_err_to_name(ret));
    }
}

struct RowUi
{
    lv_obj_t *row = nullptr;
    lv_obj_t *name = nullptr;
    lv_obj_t *kind = nullptr;
};

struct BrowserLoadState
{
    BrowserLoadPhase phase = BrowserLoadPhase::Idle;
    VideoBrowser::DirectoryScanSession scan = {};
    uint32_t last_wait_log_ms = 0U;
};

struct CadenceProbeWindow
{
    uint32_t ticks = 0U;
    uint32_t tick_gap_samples = 0U;
    uint64_t tick_gap_us_total = 0ULL;
    uint32_t tick_gap_us_max = 0U;
    uint32_t take_miss = 0U;
    uint32_t arm_wait = 0U;

    uint32_t ready_samples = 0U;
    int64_t ready_late_us_total = 0LL;
    int32_t ready_late_us_min = 0;
    int32_t ready_late_us_max = 0;

    uint32_t held_samples = 0U;
    uint64_t held_wait_us_total = 0ULL;
    uint32_t held_wait_us_max = 0U;

    uint32_t present_start_samples = 0U;
    int64_t present_start_late_us_total = 0LL;
    int32_t present_start_late_us_min = 0;
    int32_t present_start_late_us_max = 0;
    uint32_t present_done_samples = 0U;
    int64_t present_done_late_us_total = 0LL;
    int32_t present_done_late_us_min = 0;
    int32_t present_done_late_us_max = 0;

    uint32_t present_gap_samples = 0U;
    uint64_t present_gap_us_total = 0ULL;
    uint32_t present_gap_us_max = 0U;
    uint32_t post_idle_samples = 0U;
    uint64_t post_idle_us_total = 0ULL;
    uint32_t post_idle_us_max = 0U;
};

struct BenchmarkUiState
{
    VideoBenchmark::FrameView held = {};
    bool has_held = false;
    bool bounded_session = false;
    bool clock_started = false;
    bool audio_master_active = false;
    uint32_t audio_first_pts_ms = 0U;
    bool terminal_shown = false;
    bool cleanup_pending = false;
    uint32_t first_pts_ms = 0U;
    int64_t clock_base_us = 0LL;
    int64_t first_present_us = 0LL;
    int64_t last_present_us = 0LL;
    uint32_t presented = 0U;
    uint32_t dropped = 0U;
    uint32_t display_failures = 0U;
    esp_err_t display_error = ESP_OK;
    uint64_t display_us_total = 0ULL;
    uint32_t display_us_max = 0U;
    uint64_t te_us_total = 0ULL;
    uint32_t te_us_max = 0U;
    uint64_t stream_us_total = 0ULL;
    uint32_t stream_us_max = 0U;

    CadenceProbeWindow probe = {};
};

static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_header_back = nullptr;
static lv_obj_t *g_header_title = nullptr;
static lv_obj_t *g_header_line = nullptr;
static lv_obj_t *g_browser_host = nullptr;
static lv_obj_t *g_browser_status = nullptr;
static lv_obj_t *g_browser_position = nullptr;
static RowUi g_rows[kVisibleRows] = {};
static size_t g_first_index = 0U;
static lv_obj_t *g_probe_host = nullptr;
static lv_obj_t *g_probe_title = nullptr;
static lv_obj_t *g_probe_video = nullptr;
static lv_obj_t *g_probe_audio = nullptr;
static lv_obj_t *g_probe_misc = nullptr;
static lv_obj_t *g_risk_continue = nullptr;
static lv_obj_t *g_risk_cancel = nullptr;
static lv_timer_t *g_timer = nullptr;
static VideoPage g_page = VideoPage::Browser;
static VideoBrowser::DirectorySnapshot g_directory = {};
static BrowserLoadState g_browser_load = {};
static char *g_current_dir = nullptr;
static char *g_scratch_path = nullptr;
static char *g_selected_path = nullptr;
static BenchmarkUiState g_benchmark_ui = {};
static portMUX_TYPE g_benchmark_ui_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t g_present_task = nullptr;
static bool g_present_running = false;
static bool g_present_stop_requested = false;
static bool g_music_paused_for_benchmark = false;
static bool g_music_deep_suspended_for_benchmark = false;
static bool g_fullcanvas_risk_authorized = false;
static bool g_profile_probe_pending = false;
static bool g_risk_prompt_active = false;
static size_t g_selected_index = kInvalidEntryIndex;
static EofTransition g_eof_transition = EofTransition::None;

// R46.0.29：BLE 只提交媒体意图；Video timer 在 LVGL/Presenter 所属上下文中串行执行。
static portMUX_TYPE g_remote_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_remote_context_active = false;
static VideoRemoteCommand g_remote_pending = VideoRemoteCommand::TogglePlayPause;
static bool g_remote_pending_valid = false;
static uint32_t g_remote_playback_revision = 1U;
static uint32_t g_remote_metadata_revision = 1U;
static uint32_t g_remote_item_index = UINT32_MAX;
static char g_remote_title[128] = {};

static bool start_profile_probe_selected();
static void benchmark_show_terminal(const VideoBenchmark::Snapshot &snapshot);
static const char *basename_of(const char *path);

static void remote_bump_revision(uint32_t *revision)
{
    if (revision == nullptr) return;
    ++(*revision);
    if (*revision == 0U) ++(*revision);
}

static void remote_context_begin()
{
    char title[sizeof(g_remote_title)] = {};
    snprintf(title, sizeof(title), "%s", basename_of(g_selected_path));
    const uint32_t item_index = g_selected_index == kInvalidEntryIndex
        ? UINT32_MAX : static_cast<uint32_t>(g_selected_index);
    portENTER_CRITICAL(&g_remote_mux);
    g_remote_context_active = true;
    g_remote_item_index = item_index;
    memcpy(g_remote_title, title, sizeof(g_remote_title));
    remote_bump_revision(&g_remote_playback_revision);
    remote_bump_revision(&g_remote_metadata_revision);
    portEXIT_CRITICAL(&g_remote_mux);
}

static void remote_identity_changed()
{
    char title[sizeof(g_remote_title)] = {};
    snprintf(title, sizeof(title), "%s", basename_of(g_selected_path));
    const uint32_t item_index = g_selected_index == kInvalidEntryIndex
        ? UINT32_MAX : static_cast<uint32_t>(g_selected_index);
    portENTER_CRITICAL(&g_remote_mux);
    g_remote_item_index = item_index;
    memcpy(g_remote_title, title, sizeof(g_remote_title));
    remote_bump_revision(&g_remote_playback_revision);
    remote_bump_revision(&g_remote_metadata_revision);
    portEXIT_CRITICAL(&g_remote_mux);
}

static void remote_playback_changed()
{
    portENTER_CRITICAL(&g_remote_mux);
    remote_bump_revision(&g_remote_playback_revision);
    portEXIT_CRITICAL(&g_remote_mux);
}

static void remote_context_end()
{
    portENTER_CRITICAL(&g_remote_mux);
    if (g_remote_context_active) {
        g_remote_context_active = false;
        g_remote_pending_valid = false;
        remote_bump_revision(&g_remote_playback_revision);
        remote_bump_revision(&g_remote_metadata_revision);
    }
    portEXIT_CRITICAL(&g_remote_mux);
}

static bool remote_take_pending(VideoRemoteCommand *out_command)
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

static esp_err_t cleanup_create_failure(esp_err_t err)
{
    if (g_timer != nullptr) { lv_timer_delete(g_timer); g_timer = nullptr; }
    if (g_root != nullptr) { lv_obj_delete(g_root); g_root = nullptr; }
    g_header_back = nullptr;
    g_header_title = nullptr;
    g_header_line = nullptr;
    g_browser_host = nullptr;
    g_browser_status = nullptr;
    g_browser_position = nullptr;
    for (RowUi &ui : g_rows) ui = {};
    g_probe_host = nullptr;
    g_probe_title = nullptr;
    g_probe_video = nullptr;
    g_probe_audio = nullptr;
    g_probe_misc = nullptr;
    g_risk_continue = nullptr;
    g_risk_cancel = nullptr;
    if (g_current_dir != nullptr) heap_caps_free(g_current_dir);
    if (g_scratch_path != nullptr) heap_caps_free(g_scratch_path);
    if (g_selected_path != nullptr) heap_caps_free(g_selected_path);
    g_current_dir = nullptr;
    g_scratch_path = nullptr;
    g_selected_path = nullptr;
    g_selected_index = kInvalidEntryIndex;
    g_eof_transition = EofTransition::None;
    g_page = VideoPage::Browser;
    gesture_router_set_video_seek_mode(false);
    return err;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t rgb, lv_text_align_t align)
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

static const char *basename_of(const char *path)
{
    if (path == nullptr || path[0] == '\0') return "视频";
    const char *slash = strrchr(path, '/');
    return slash != nullptr && slash[1] != '\0' ? slash + 1 : path;
}

static bool flac_storage_safe(bool *out_competing = nullptr, uint32_t *out_percent = nullptr)
{
    if (out_competing != nullptr) *out_competing = false;
    if (out_percent != nullptr) *out_percent = 100U;
    FlacStorageWindowSnapshot window = {};
    if (!flac_decoder_get_storage_window(&window) || !window.active ||
        !window.storage_competes || window.capacity_bytes == 0U) return true;
    const uint32_t percent = static_cast<uint32_t>(
        (static_cast<uint64_t>(window.buffered_bytes) * 100ULL + window.capacity_bytes / 2ULL) /
        window.capacity_bytes);
    if (out_competing != nullptr) *out_competing = true;
    if (out_percent != nullptr) *out_percent = percent;
    return percent >= kFlacSafePercent;
}

static bool benchmark_music_exclusive_active()
{
    return g_music_paused_for_benchmark || g_music_deep_suspended_for_benchmark;
}

static bool benchmark_pause_music_for_exclusive()
{
    player_transport_cancel_deferred_nas_play("进入Video独占播放");
    if (benchmark_music_exclusive_active()) return true;

    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        ESP_LOGI(TAG, "Video Exclusive：AudioTask未就绪，无活动Music需要暂停");
        return true;
    }
    if (snapshot.state == AudioPlaybackState::Playing) {
        if (!audio_service_pause(true)) {
            ESP_LOGE(TAG, "Video Exclusive：暂停Music失败，取消AVI启动");
            return false;
        }
        g_music_paused_for_benchmark = true;
        if (!audio_service_get_snapshot(&snapshot)) {
            ESP_LOGW(TAG, "Video Exclusive：暂停后读取Music状态失败，本次保持浅暂停");
            return true;
        }
    } else if (snapshot.state != AudioPlaybackState::Paused) {
        ESP_LOGI(TAG, "Video Exclusive：Music当前非Playing/Paused(state=%u)，保持原状态",
            static_cast<unsigned>(snapshot.state));
        return true;
    }
    g_music_deep_suspended_for_benchmark = false;

    // Local 仍要求可靠 Seek；NAS 顺序 HTTP 则彻底释放并在退出 Video 后从 00:00 重建。
    const size_t player_track = player_state_get_index();
    const char *path = player_state_get_path();
    MediaTechnicalInfo technical = {};
    const bool has_technical = player_state_get_technical_info(&technical);
    const bool mp3_index_safe = snapshot.format != MediaFormat::MP3 ||
        (has_technical && (technical.flags & MEDIA_TECH_PARSED) != 0U);
    const AudioPlaybackSource player_source = player_state_get_source() == PlayerMediaSource::Nas
        ? AudioPlaybackSource::NasHttp : AudioPlaybackSource::Local;
    const bool context_matches = player_state_is_ready() && path != nullptr && path[0] != '\0' &&
        player_track == snapshot.track_index && player_state_get_format() == snapshot.format &&
        player_source == snapshot.source;
    const bool nas_restart_from_beginning =
        snapshot.source == AudioPlaybackSource::NasHttp &&
        (snapshot.format == MediaFormat::MP3 || snapshot.format == MediaFormat::FLAC);
    const bool local_seek_restore = snapshot.source == AudioPlaybackSource::Local &&
        snapshot.seek_supported && mp3_index_safe;

    if ((nas_restart_from_beginning || local_seek_restore) && context_matches) {
        if (audio_service_music_deep_suspend(
                AudioMusicSuspendOwner::Video,
                snapshot.track_index,
                path,
                snapshot.format,
                has_technical ? &technical : nullptr,
                true)) {
            g_music_deep_suspended_for_benchmark = true;
            ESP_LOGI(TAG,
                "Video Exclusive：Music已深度挂起；source=%s restore=%s decoder/source/read-ahead/workspace已回收",
                nas_restart_from_beginning ? "NAS_HTTP" : "LOCAL",
                nas_restart_from_beginning ? "00:00" : "原位置");
        } else {
            ESP_LOGW(TAG, "Video Exclusive：Music深度挂起不可用，本次保持浅暂停继续播放Video");
        }
    } else {
        ESP_LOGI(TAG,
            "Video Exclusive：Music保持浅暂停 source=%u seek=%u context=%u mp3_index=%u format=%s",
            static_cast<unsigned>(snapshot.source),
            static_cast<unsigned>(snapshot.seek_supported),
            static_cast<unsigned>(context_matches),
            static_cast<unsigned>(mp3_index_safe),
            media_format_name(snapshot.format));
    }

    ESP_LOGI(TAG, "Video Exclusive：Music已进入独占暂停；AVI接管TF/Decode/Presenter性能窗口");
    return true;
}

static void benchmark_restore_music_after_exclusive(const char *reason)
{
    if (!benchmark_music_exclusive_active()) return;
    if (g_music_deep_suspended_for_benchmark) {
        AudioStateSnapshot music = {};
        if (audio_service_get_snapshot(&music) && music.source == AudioPlaybackSource::NasHttp) {
            if (!audio_service_music_deep_restore_async(AudioMusicSuspendOwner::Video, g_music_paused_for_benchmark)) {
                ESP_LOGW(TAG, "NAS Music restore queue busy; retry exit");
                return;
            }
            g_music_deep_suspended_for_benchmark = false;
            g_music_paused_for_benchmark = false;
            ESP_LOGI(TAG, "NAS Music restore queued; screen exit does not wait for network");
            return;
        }

        if (!audio_service_music_deep_restore(AudioMusicSuspendOwner::Video, true)) {
            ESP_LOGW(TAG, "Video Exclusive：深度恢复Music失败 reason=%s；保留上下文等待生命周期重试",
                reason != nullptr ? reason : "unknown");
            return;
        }
        g_music_deep_suspended_for_benchmark = false;
    }
    if (g_music_paused_for_benchmark) {
        if (!audio_service_resume(false)) {
            ESP_LOGW(TAG, "Video Exclusive：恢复Music请求失败 reason=%s；保留暂停标记等待后续生命周期重试",
                reason != nullptr ? reason : "unknown");
            return;
        }
        g_music_paused_for_benchmark = false;
        ESP_LOGI(TAG, "Video Exclusive：已请求恢复Music reason=%s",
            reason != nullptr ? reason : "unknown");
    } else {
        ESP_LOGI(TAG, "Video Exclusive：Music进入Video前原本Paused，深度恢复后继续保持Paused reason=%s",
            reason != nullptr ? reason : "unknown");
    }
}

static bool benchmark_stop_avi_audio(const char *reason)
{
    if (audio_service_video_mp3_stop(true)) return true;
    ESP_LOGE(TAG, "AVI MP3 AudioTask停止/恢复Paused Music硬件失败：reason=%s",
        reason != nullptr ? reason : "unknown");
    return false;
}

static void set_visible(lv_obj_t *obj, bool visible)
{
    if (obj == nullptr) return;
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void update_header()
{
    if (g_header_title == nullptr) return;
    if (g_page == VideoPage::Benchmark) {
        lv_label_set_text(g_header_title, g_risk_prompt_active ? "视频" : "MJPEG Benchmark");
    } else if (g_current_dir != nullptr && strcmp(g_current_dir, VideoBrowser::kRootDirectory) != 0) {
        lv_label_set_text(g_header_title, basename_of(g_current_dir));
    } else {
        lv_label_set_text(g_header_title, "视频");
    }
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
        show_status("/video 中没有 AVI", 0x7F8A99);
        return;
    }
    if (g_browser_status != nullptr) set_visible(g_browser_status, false);
    const size_t last_page_first =
        ((g_directory.count - 1U) / kVisibleRows) * kVisibleRows;
    if (g_first_index > last_page_first) g_first_index = last_page_first;

    size_t last = g_first_index;
    for (size_t slot = 0; slot < kVisibleRows; ++slot) {
        RowUi &ui = g_rows[slot];
        const size_t index = g_first_index + slot;
        if (ui.row == nullptr || ui.name == nullptr || ui.kind == nullptr || index >= g_directory.count) {
            if (ui.row != nullptr) set_visible(ui.row, false);
            continue;
        }
        const VideoBrowser::EntryIndex *entry = VideoBrowser::entry_at(&g_directory, index);
        const char *name = VideoBrowser::entry_name(&g_directory, index);
        if (entry == nullptr || name == nullptr) { set_visible(ui.row, false); continue; }
        const bool is_dir = VideoBrowser::entry_is_directory(entry);
        lv_label_set_text(ui.name, name);
        lv_label_set_text(ui.kind, is_dir ? ">" : "AVI");
        lv_obj_set_style_text_color(ui.name, lv_color_hex(is_dir ? 0xBCD7FF : 0xEEF1F5), 0);
        lv_obj_set_style_text_color(ui.kind, lv_color_hex(is_dir ? 0x6EA5F2 : 0x677385), 0);
        lv_obj_set_style_bg_color(ui.row, lv_color_hex(is_dir ? 0x172230 : 0x151A21), 0);
        set_visible(ui.row, true);
        last = index;
    }
    if (g_browser_position != nullptr) {
        char pos[64] = {};
        snprintf(pos, sizeof(pos), "%u-%u / %u",
            static_cast<unsigned>(g_first_index + 1U),
            static_cast<unsigned>(last + 1U),
            static_cast<unsigned>(g_directory.count));
        lv_label_set_text(g_browser_position, pos);
        set_visible(g_browser_position, true);
    }
}

static void shift_window(int direction)
{
    if (g_page != VideoPage::Browser || g_browser_load.phase != BrowserLoadPhase::Idle ||
        g_directory.entries == nullptr || g_directory.count <= kVisibleRows || direction == 0) return;
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
    ESP_LOGI(TAG, "Video目录分页：first=%u total=%u direction=%s",
        static_cast<unsigned>(g_first_index), static_cast<unsigned>(g_directory.count),
        direction > 0 ? "NEXT" : "PREV");
}

static void cancel_browser_load()
{
    VideoBrowser::cancel_directory_scan(&g_browser_load.scan);
    g_browser_load = {};
}

static void begin_browser_load()
{
    cancel_browser_load();
    VideoBrowser::release_directory(&g_directory);
    g_first_index = 0U;
    g_selected_index = kInvalidEntryIndex;
    g_browser_load.phase = BrowserLoadPhase::WaitingForAudioWindow;
    show_status("正在加载视频目录…", 0x8E9AAA);
    ESP_LOGI(TAG, "Video目录协作加载已排队：%s", g_current_dir != nullptr ? g_current_dir : "(null)");
}

static void finish_browser_load()
{
    const esp_err_t ret = VideoBrowser::finish_directory_scan(&g_browser_load.scan, &g_directory);
    g_browser_load = {};
    if (ret != ESP_OK) {
        show_status("视频目录加载失败", 0xE18A8A);
        ESP_LOGW(TAG, "Video目录完成失败：%s", esp_err_to_name(ret));
        return;
    }
    update_rows();
    ESP_LOGI(TAG, "Video目录索引：%s items=%u index=%uB strings=%uB virtual_rows=%u",
        g_current_dir,
        static_cast<unsigned>(g_directory.count),
        static_cast<unsigned>(g_directory.count * sizeof(VideoBrowser::EntryIndex)),
        static_cast<unsigned>(g_directory.pool_size),
        static_cast<unsigned>(kVisibleRows));
}

static void browser_load_tick()
{
    if (g_page != VideoPage::Browser || g_browser_load.phase == BrowserLoadPhase::Idle ||
        g_current_dir == nullptr || app_manager_foreground() != AppId::Video ||
        app_launcher_overlay_is_visible()) return;

    bool competing = false;
    uint32_t percent = 100U;
    if (!flac_storage_safe(&competing, &percent)) {
        const uint32_t now = static_cast<uint32_t>(lv_tick_get());
        if (now - g_browser_load.last_wait_log_ms >= kWaitLogIntervalMs) {
            g_browser_load.last_wait_log_ms = now;
            ESP_LOGI(TAG, "Video目录加载让路FLAC：ring=%u%% < %u%% phase=%u",
                static_cast<unsigned>(percent), static_cast<unsigned>(kFlacSafePercent),
                static_cast<unsigned>(g_browser_load.phase));
        }
        return;
    }

    if (g_browser_load.phase == BrowserLoadPhase::WaitingForAudioWindow) {
        const esp_err_t ret = VideoBrowser::begin_directory_scan(g_current_dir, &g_browser_load.scan);
        if (ret != ESP_OK) {
            g_browser_load = {};
            show_status(ret == ESP_ERR_NOT_FOUND ? "请在TF卡根目录创建 video 文件夹" : "视频目录打开失败", 0xE18A8A);
            ESP_LOGW(TAG, "Video目录打开失败：path=%s ret=%s", g_current_dir, esp_err_to_name(ret));
            return;
        }
        g_browser_load.phase = BrowserLoadPhase::Scanning;
    }

    bool done = false;
    const size_t batch = competing ? kScanBatchWithFlac : kScanBatchNoFlac;
    const esp_err_t ret = VideoBrowser::scan_directory_step(&g_browser_load.scan, batch, &done);
    if (ret == ESP_ERR_TIMEOUT) return;
    if (ret != ESP_OK) {
        cancel_browser_load();
        show_status("视频目录扫描失败", 0xE18A8A);
        ESP_LOGW(TAG, "Video目录扫描失败：%s", esp_err_to_name(ret));
        return;
    }
    if (done) finish_browser_load();
}

static void show_browser()
{
    benchmark_controls_hide();
    gesture_router_set_single_tap_enabled(false);
    remote_context_end();
    g_page = VideoPage::Browser;
    g_fullcanvas_risk_authorized = false;
    g_risk_prompt_active = false;
    gesture_router_set_video_seek_mode(false);
    if (g_timer != nullptr) lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
    set_visible(g_root, true);
    set_visible(g_browser_host, true);
    set_visible(g_probe_host, false);
    set_visible(g_risk_continue, false);
    set_visible(g_risk_cancel, false);
    update_header();
    // Profile Probe 状态页会隐藏5行列表和位置标签；返回Browser时必须恢复列表可见状态。
    update_rows();
    gesture_router_reset();
}

static BenchmarkUiState benchmark_ui_snapshot()
{
    BenchmarkUiState snapshot = {};
    portENTER_CRITICAL(&g_benchmark_ui_mux);
    snapshot = g_benchmark_ui;
    portEXIT_CRITICAL(&g_benchmark_ui_mux);
    return snapshot;
}

static bool benchmark_presenter_should_stop()
{
    bool stop = false;
    portENTER_CRITICAL(&g_benchmark_ui_mux);
    stop = g_present_stop_requested;
    portEXIT_CRITICAL(&g_benchmark_ui_mux);
    return stop;
}

static bool benchmark_presenter_is_running()
{
    bool running = false;
    portENTER_CRITICAL(&g_benchmark_ui_mux);
    running = g_present_running;
    portEXIT_CRITICAL(&g_benchmark_ui_mux);
    return running;
}

static void benchmark_presenter_request_stop()
{
    portENTER_CRITICAL(&g_benchmark_ui_mux);
    g_present_stop_requested = true;
    portEXIT_CRITICAL(&g_benchmark_ui_mux);
}

static bool benchmark_presenter_wait_stopped(uint32_t timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000LL;
    TickType_t wait_tick = pdMS_TO_TICKS(5);
    if (wait_tick == 0) wait_tick = 1;
    while (benchmark_presenter_is_running()) {
        if (esp_timer_get_time() >= deadline_us) return false;
        vTaskDelay(wait_tick);
    }
    return true;
}

// SEEK 完成后的短暂浮层：底部进度条 + “当前时间 / 总时长”，显示 2 秒后自动消失。
// 浮层直接叠加在 Presenter 合成的 RGB565 帧上，不暂停推流、不影响音频管线。
static constexpr int64_t kSeekOverlayHoldUs = 2000000LL;
static constexpr uint16_t kSeekOverlayMarginX = 40U;
static constexpr uint16_t kSeekOverlayPanelHeight = 48U;
static constexpr uint16_t kSeekOverlayBottomGap = 18U;
static constexpr uint16_t kSeekOverlayGlyphScale = 3U;

struct SeekOverlayState
{
    bool active = false;
    int64_t until_us = 0LL;
    uint32_t target_ms = 0U;
    uint32_t duration_ms = 0U;
};

static SeekOverlayState g_seek_overlay = {};
static portMUX_TYPE g_seek_overlay_mux = portMUX_INITIALIZER_UNLOCKED;

static void benchmark_seek_overlay_show(
    uint32_t target_ms,
    uint32_t duration_ms,
    int64_t hold_us = kSeekOverlayHoldUs)
{
    portENTER_CRITICAL(&g_seek_overlay_mux);
    g_seek_overlay.active = true;
    g_seek_overlay.until_us = esp_timer_get_time() + hold_us;
    g_seek_overlay.target_ms = target_ms;
    g_seek_overlay.duration_ms = duration_ms;
    portEXIT_CRITICAL(&g_seek_overlay_mux);
}

// Presenter 每帧取一次快照；超过 2 秒自动失效，不需要额外的 UI 定时器。
static bool benchmark_seek_overlay_snapshot(uint32_t *out_target_ms, uint32_t *out_duration_ms)
{
    bool active = false;
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&g_seek_overlay_mux);
    if (g_seek_overlay.active) {
        if (now_us >= g_seek_overlay.until_us) {
            g_seek_overlay.active = false;
        } else {
            active = true;
            if (out_target_ms != nullptr) *out_target_ms = g_seek_overlay.target_ms;
            if (out_duration_ms != nullptr) *out_duration_ms = g_seek_overlay.duration_ms;
        }
    }
    portEXIT_CRITICAL(&g_seek_overlay_mux);
    return active;
}

// 3x5 点阵数字，bit2=最左列。只覆盖时间显示需要的字符。
static const uint8_t kSeekGlyphDigits[10][5] = {
    {0x7, 0x5, 0x5, 0x5, 0x7}, // 0
    {0x2, 0x6, 0x2, 0x2, 0x7}, // 1
    {0x7, 0x1, 0x7, 0x4, 0x7}, // 2
    {0x7, 0x1, 0x7, 0x1, 0x7}, // 3
    {0x5, 0x5, 0x7, 0x1, 0x1}, // 4
    {0x7, 0x4, 0x7, 0x1, 0x7}, // 5
    {0x7, 0x4, 0x7, 0x5, 0x7}, // 6
    {0x7, 0x1, 0x1, 0x1, 0x1}, // 7
    {0x7, 0x5, 0x7, 0x5, 0x7}, // 8
    {0x7, 0x5, 0x7, 0x1, 0x7}, // 9
};
static const uint8_t kSeekGlyphColon[5] = {0x0, 0x2, 0x0, 0x2, 0x0};
static const uint8_t kSeekGlyphSlash[5] = {0x1, 0x1, 0x2, 0x4, 0x4};

static const uint8_t *benchmark_seek_glyph(char c)
{
    if (c >= '0' && c <= '9') return kSeekGlyphDigits[c - '0'];
    if (c == ':') return kSeekGlyphColon;
    if (c == '/') return kSeekGlyphSlash;
    return nullptr; // 空格等不绘制
}

// dst 与 rgb565_be 同字节序（大端），这里按大端读写像素。
static inline void benchmark_seek_put_be(uint8_t *row, uint16_t x, uint16_t rgb565)
{
    row[x * 2U] = static_cast<uint8_t>((rgb565 >> 8) & 0xFFU);
    row[x * 2U + 1U] = static_cast<uint8_t>(rgb565 & 0xFFU);
}

static inline uint16_t benchmark_seek_get_be(const uint8_t *row, uint16_t x)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(row[x * 2U]) << 8) |
        static_cast<uint16_t>(row[x * 2U + 1U]));
}

// 底衬暗化：R/G/B 各取半，近似半透明黑色遮罩。
static inline uint16_t benchmark_seek_dim(uint16_t rgb565)
{
    const uint16_t r = static_cast<uint16_t>(((rgb565 >> 11) & 0x1FU) >> 1);
    const uint16_t g = static_cast<uint16_t>(((rgb565 >> 5) & 0x3FU) >> 1);
    const uint16_t b = static_cast<uint16_t>((rgb565 & 0x1FU) >> 1);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

static void benchmark_seek_overlay_draw(
    uint8_t *dst,
    uint16_t strip_y,
    uint16_t rows,
    uint16_t width,
    uint16_t height,
    uint32_t target_ms,
    uint32_t duration_ms)
{
    if (dst == nullptr || rows == 0U || width == 0U) return;
    if (width < kSeekOverlayMarginX * 2U + 8U) return;
    if (height < kSeekOverlayPanelHeight + 4U) return;

    const uint16_t panel_left = kSeekOverlayMarginX;
    const uint16_t panel_right = static_cast<uint16_t>(width - kSeekOverlayMarginX);
    const uint16_t panel_bottom = static_cast<uint16_t>(height - kSeekOverlayBottomGap);
    const uint16_t panel_top = static_cast<uint16_t>(panel_bottom - kSeekOverlayPanelHeight);
    if (strip_y + rows <= panel_top || strip_y >= panel_bottom) return;

    char text[32] = {};
    const uint32_t target_s = target_ms / 1000U;
    if (duration_ms > 0U) {
        const uint32_t duration_s = duration_ms / 1000U;
        snprintf(text, sizeof(text), "%u:%02u / %u:%02u",
            static_cast<unsigned>(target_s / 60U), static_cast<unsigned>(target_s % 60U),
            static_cast<unsigned>(duration_s / 60U), static_cast<unsigned>(duration_s % 60U));
    } else {
        snprintf(text, sizeof(text), "%u:%02u",
            static_cast<unsigned>(target_s / 60U), static_cast<unsigned>(target_s % 60U));
    }

    const uint16_t glyph_width = 3U * kSeekOverlayGlyphScale;
    const uint16_t glyph_height = 5U * kSeekOverlayGlyphScale;
    const uint16_t advance = glyph_width + kSeekOverlayGlyphScale;
    const uint16_t text_width = static_cast<uint16_t>(
        strlen(text) * static_cast<size_t>(advance));
    const uint16_t panel_width = static_cast<uint16_t>(panel_right - panel_left);
    uint16_t text_left = panel_left;
    if (text_width < panel_width) {
        text_left = static_cast<uint16_t>(panel_left + (panel_width - text_width) / 2U);
    }
    const uint16_t text_top = static_cast<uint16_t>(panel_top + 6U);
    const uint16_t bar_top = static_cast<uint16_t>(panel_bottom - 12U);
    const uint16_t bar_bottom = static_cast<uint16_t>(panel_bottom - 6U);

    uint32_t percent = 0U;
    if (duration_ms > 0U) {
        percent = static_cast<uint32_t>(
            static_cast<uint64_t>(target_ms) * 100ULL / duration_ms);
        if (percent > 100U) percent = 100U;
    }
    const uint16_t progress_x = static_cast<uint16_t>(
        panel_left + static_cast<uint32_t>(panel_width) * percent / 100U);

    const size_t text_len = strlen(text);
    for (uint16_t r = 0; r < rows; ++r) {
        const uint16_t y = static_cast<uint16_t>(strip_y + r);
        if (y < panel_top || y >= panel_bottom) continue;
        uint8_t *row = dst + static_cast<size_t>(r) * width * 2U;

        for (uint16_t x = panel_left; x < panel_right; ++x) {
            benchmark_seek_put_be(row, x, benchmark_seek_dim(benchmark_seek_get_be(row, x)));
        }

        if (y >= bar_top && y < bar_bottom) {
            for (uint16_t x = panel_left; x < panel_right; ++x) {
                benchmark_seek_put_be(row, x, x < progress_x ? 0xFFFFU : 0x4208U);
            }
        }

        if (y >= text_top && y < text_top + glyph_height) {
            const uint16_t dot_row = static_cast<uint16_t>((y - text_top) / kSeekOverlayGlyphScale);
            for (size_t i = 0; i < text_len; ++i) {
                const uint8_t *glyph = benchmark_seek_glyph(text[i]);
                if (glyph == nullptr) continue;
                const uint16_t glyph_left = static_cast<uint16_t>(text_left + i * advance);
                for (uint16_t col = 0; col < glyph_width; ++col) {
                    const uint16_t dot_col = static_cast<uint16_t>(col / kSeekOverlayGlyphScale);
                    if ((glyph[dot_row] & (0x04U >> dot_col)) == 0U) continue;
                    const uint16_t x = static_cast<uint16_t>(glyph_left + col);
                    if (x < panel_left || x >= panel_right) continue;
                    benchmark_seek_put_be(row, x, 0xFFFFU);
                }
            }
        }
    }
}


// Video controls are composited into the Presenter RGB565 stream. LVGL never flushes
// over the active video frame, so the overlay cannot race BoundedSPI.
static void benchmark_controls_fill_rect(
    uint8_t *dst, uint16_t strip_y, uint16_t rows, uint16_t width,
    int32_t left, int32_t top, int32_t right, int32_t bottom, uint16_t color)
{
    if (dst == nullptr || rows == 0U || width == 0U || right <= left || bottom <= top) return;
    if (left < 0) left = 0;
    if (right > width) right = width;
    const int32_t strip_bottom = static_cast<int32_t>(strip_y) + rows;
    if (top < strip_y) top = strip_y;
    if (bottom > strip_bottom) bottom = strip_bottom;
    if (right <= left || bottom <= top) return;
    for (int32_t y = top; y < bottom; ++y) {
        uint8_t *row = dst + static_cast<size_t>(y - strip_y) * width * 2U;
        for (int32_t x = left; x < right; ++x) {
            benchmark_seek_put_be(row, static_cast<uint16_t>(x), color);
        }
    }
}

static void benchmark_controls_fill_round_rect(
    uint8_t *dst, uint16_t strip_y, uint16_t rows, uint16_t width,
    int32_t left, int32_t top, int32_t right, int32_t bottom,
    int32_t radius, uint16_t color)
{
    if (right <= left || bottom <= top) return;
    const int32_t strip_bottom = static_cast<int32_t>(strip_y) + rows;
    const int32_t y_begin = top > strip_y ? top : strip_y;
    const int32_t y_end = bottom < strip_bottom ? bottom : strip_bottom;
    for (int32_t y = y_begin; y < y_end; ++y) {
        const int32_t from_top = y - top;
        const int32_t from_bottom = bottom - 1 - y;
        int32_t inset = 0;
        if (from_top < radius) inset = radius - from_top;
        if (from_bottom < radius && radius - from_bottom > inset) inset = radius - from_bottom;
        benchmark_controls_fill_rect(
            dst, strip_y, rows, width, left + inset, y, right - inset, y + 1, color);
    }
}

static const uint8_t kVideoGlyphB[5] = {0x6, 0x5, 0x6, 0x5, 0x6};
static const uint8_t kVideoGlyphR[5] = {0x6, 0x5, 0x6, 0x5, 0x5};
static const uint8_t kVideoGlyphI[5] = {0x7, 0x2, 0x2, 0x2, 0x7};
static const uint8_t kVideoGlyphV[5] = {0x5, 0x5, 0x5, 0x5, 0x2};
static const uint8_t kVideoGlyphO[5] = {0x7, 0x5, 0x5, 0x5, 0x7};
static const uint8_t kVideoGlyphL[5] = {0x4, 0x4, 0x4, 0x4, 0x7};
static const uint8_t kVideoGlyphPercent[5] = {0x5, 0x1, 0x2, 0x4, 0x5};

static const uint8_t *benchmark_controls_glyph(char c)
{
    if (c >= '0' && c <= '9') return kSeekGlyphDigits[c - '0'];
    switch (c) {
        case 'B': return kVideoGlyphB;
        case 'R': return kVideoGlyphR;
        case 'I': return kVideoGlyphI;
        case 'V': return kVideoGlyphV;
        case 'O': return kVideoGlyphO;
        case 'L': return kVideoGlyphL;
        case '%': return kVideoGlyphPercent;
        default: return nullptr;
    }
}

static uint16_t benchmark_controls_text_width(const char *text, uint16_t scale)
{
    if (text == nullptr || text[0] == '\0') return 0U;
    const size_t count = strlen(text);
    const size_t width = count * static_cast<size_t>(4U * scale) - scale;
    return width > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(width);
}

static void benchmark_controls_draw_text(
    uint8_t *dst, uint16_t strip_y, uint16_t rows, uint16_t width,
    uint16_t left, uint16_t top, const char *text, uint16_t scale, uint16_t color)
{
    if (text == nullptr || scale == 0U) return;
    uint32_t cursor_x = left;
    for (size_t i = 0U; text[i] != '\0'; ++i) {
        const uint8_t *glyph = benchmark_controls_glyph(text[i]);
        if (glyph != nullptr) {
            for (uint16_t row = 0U; row < 5U; ++row) {
                for (uint16_t col = 0U; col < 3U; ++col) {
                    if ((glyph[row] & (0x04U >> col)) == 0U) continue;
                    const int32_t x = static_cast<int32_t>(cursor_x + col * scale);
                    const int32_t y = static_cast<int32_t>(top + row * scale);
                    benchmark_controls_fill_rect(
                        dst, strip_y, rows, width, x, y, x + scale, y + scale, color);
                }
            }
        }
        cursor_x += 4U * scale;
    }
}

static void benchmark_controls_draw_sun(
    uint8_t *dst, uint16_t strip_y, uint16_t rows, uint16_t width, uint16_t x, uint16_t y)
{
    constexpr uint16_t yellow = 0xFFE0U;
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x + 5, y + 5, x + 10, y + 10, yellow);
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x + 7, y, x + 8, y + 4, yellow);
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x + 7, y + 11, x + 8, y + 15, yellow);
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x, y + 7, x + 4, y + 8, yellow);
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x + 11, y + 7, x + 15, y + 8, yellow);
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x + 2, y + 2, x + 4, y + 4, yellow);
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x + 11, y + 2, x + 13, y + 4, yellow);
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x + 2, y + 11, x + 4, y + 13, yellow);
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x + 11, y + 11, x + 13, y + 13, yellow);
}

static void benchmark_controls_draw_speaker(
    uint8_t *dst, uint16_t strip_y, uint16_t rows, uint16_t width, uint16_t x, uint16_t y)
{
    constexpr uint16_t white = 0xFFFFU;
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x, y + 5, x + 4, y + 11, white);
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x + 4, y + 3, x + 7, y + 13, white);
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x + 7, y + 4, x + 9, y + 12, white);
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x + 11, y + 5, x + 12, y + 11, white);
    benchmark_controls_fill_rect(dst, strip_y, rows, width, x + 14, y + 3, x + 15, y + 13, white);
}

static void benchmark_controls_draw_pill(
    uint8_t *dst, uint16_t strip_y, uint16_t rows, uint16_t width,
    uint16_t left, uint16_t top, uint16_t pill_width, uint16_t pill_height,
    bool selected, bool brightness, uint8_t percent)
{
    const uint16_t border = selected ? 0x07FFU : 0x6B4DU;
    benchmark_controls_fill_round_rect(
        dst, strip_y, rows, width, left, top, left + pill_width, top + pill_height, 9, border);
    benchmark_controls_fill_round_rect(
        dst, strip_y, rows, width, left + 2U, top + 2U,
        left + pill_width - 2U, top + pill_height - 2U, 7, 0x1082U);
    if (brightness) {
        benchmark_controls_draw_sun(dst, strip_y, rows, width, left + 8U, top + 12U);
    } else {
        benchmark_controls_draw_speaker(dst, strip_y, rows, width, left + 8U, top + 12U);
    }
    benchmark_controls_draw_text(
        dst, strip_y, rows, width, left + 30U, top + 16U,
        brightness ? "BRI" : "VOL", 2U, 0xFFFFU);
    char value[8] = {};
    snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(percent));
    const uint16_t value_width = benchmark_controls_text_width(value, 2U);
    const uint16_t value_left = static_cast<uint16_t>(
        left + pill_width > value_width + 8U ? left + pill_width - value_width - 8U : left + 8U);
    benchmark_controls_draw_text(
        dst, strip_y, rows, width, value_left, top + 16U, value, 2U, 0xFFFFU);
}

static void benchmark_controls_overlay_draw(
    uint8_t *dst, uint16_t strip_y, uint16_t rows, uint16_t width,
    const VideoControlsDrawSnapshot &snapshot)
{
    const VideoControlsLayout &layout = snapshot.layout;
    if (dst == nullptr || !snapshot.active || !layout.valid) return;
    benchmark_controls_draw_pill(
        dst, strip_y, rows, width, layout.brightness_left_local, layout.pill_top_local,
        layout.pill_width, layout.pill_height,
        snapshot.selected == VideoControlTarget::Brightness, true, snapshot.brightness_percent);
    benchmark_controls_draw_pill(
        dst, strip_y, rows, width, layout.volume_left_local, layout.pill_top_local,
        layout.pill_width, layout.pill_height,
        snapshot.selected == VideoControlTarget::Volume, false, snapshot.volume_percent);

    if (snapshot.selected == VideoControlTarget::None || layout.panel_height < 64U) return;
    benchmark_controls_fill_round_rect(
        dst, strip_y, rows, width, layout.panel_left_local, layout.panel_top_local,
        layout.panel_left_local + layout.panel_width,
        layout.panel_top_local + layout.panel_height, 9, 0x07FFU);
    benchmark_controls_fill_round_rect(
        dst, strip_y, rows, width, layout.panel_left_local + 2U, layout.panel_top_local + 2U,
        layout.panel_left_local + layout.panel_width - 2U,
        layout.panel_top_local + layout.panel_height - 2U, 7, 0x1082U);

    const uint8_t value = snapshot.selected == VideoControlTarget::Brightness
        ? snapshot.brightness_percent : snapshot.volume_percent;
    char value_text[8] = {};
    snprintf(value_text, sizeof(value_text), "%u%%", static_cast<unsigned>(value));
    const uint16_t text_width = benchmark_controls_text_width(value_text, 3U);
    const uint16_t text_left = static_cast<uint16_t>(
        layout.panel_left_local + (layout.panel_width > text_width
            ? (layout.panel_width - text_width) / 2U : 4U));
    benchmark_controls_draw_text(
        dst, strip_y, rows, width, text_left, layout.panel_top_local + 9U,
        value_text, 3U, 0xFFFFU);

    const uint16_t track_x = static_cast<uint16_t>(layout.panel_left_local + layout.panel_width / 2U - 4U);
    const uint16_t track_top = static_cast<uint16_t>(layout.panel_top_local + 34U);
    const uint16_t track_bottom = static_cast<uint16_t>(layout.panel_top_local + layout.panel_height - 12U);
    if (track_bottom <= track_top + 4U) return;
    benchmark_controls_fill_round_rect(
        dst, strip_y, rows, width, track_x, track_top, track_x + 8U, track_bottom, 4, 0x4208U);
    const uint16_t track_height = static_cast<uint16_t>(track_bottom - track_top);
    const uint16_t fill_height = static_cast<uint16_t>(
        static_cast<uint32_t>(track_height) * snapshot.slider_percent / 100U);
    const uint16_t thumb_y = static_cast<uint16_t>(track_bottom - fill_height);
    if (fill_height > 0U) {
        benchmark_controls_fill_round_rect(
            dst, strip_y, rows, width, track_x, thumb_y, track_x + 8U, track_bottom, 4, 0x07FFU);
    }
    benchmark_controls_fill_round_rect(
        dst, strip_y, rows, width, track_x - 8U, thumb_y - 3U, track_x + 16U, thumb_y + 4U, 3, 0xFFFFU);
}

static bool benchmark_controls_overlay_snapshot(
    uint16_t frame_x, uint16_t frame_y, uint16_t width, uint16_t height,
    VideoControlsDrawSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    VideoControlsDrawSnapshot snapshot = {};
    portENTER_CRITICAL(&g_video_controls_mux);
    snapshot.active = g_video_controls.active;
    snapshot.selected = g_video_controls.selected;
    snapshot.brightness_percent = g_video_controls.brightness_percent;
    snapshot.volume_percent = g_video_controls.volume_percent;
    portEXIT_CRITICAL(&g_video_controls_mux);
    if (!snapshot.active || width < 160U || height < 120U) return false;

    const uint16_t margin = width >= 320U ? 12U : 6U;
    const uint16_t gap = width >= 320U ? 12U : 8U;
    const uint16_t available = width > (margin * 2U + gap)
        ? static_cast<uint16_t>(width - margin * 2U - gap) : 0U;
    uint16_t pill_width = static_cast<uint16_t>(available / 2U);
    if (pill_width > 148U) pill_width = 148U;
    if (pill_width < 72U) return false;

    VideoControlsLayout layout = {};
    layout.valid = true;
    layout.pill_width = pill_width;
    layout.pill_height = 42U;
    layout.pill_top_local = 8U;
    layout.brightness_left_local = static_cast<uint16_t>((width - (pill_width * 2U + gap)) / 2U);
    layout.volume_left_local = static_cast<uint16_t>(layout.brightness_left_local + pill_width + gap);

    const uint16_t panel_top = static_cast<uint16_t>(layout.pill_top_local + layout.pill_height + 8U);
    const uint16_t panel_width = pill_width > 20U
        ? static_cast<uint16_t>(pill_width - 12U) : pill_width;
    uint16_t panel_height = height > panel_top + 8U
        ? static_cast<uint16_t>(height - panel_top - 8U) : 0U;
    if (panel_height > 116U) panel_height = 116U;
    if (panel_height < 64U || snapshot.selected == VideoControlTarget::None) panel_height = 0U;
    if (panel_height > 0U) {
        const uint16_t selected_left = snapshot.selected == VideoControlTarget::Brightness
            ? layout.brightness_left_local : layout.volume_left_local;
        layout.panel_width = panel_width;
        layout.panel_height = panel_height;
        layout.panel_left_local = static_cast<uint16_t>(selected_left + (pill_width - panel_width) / 2U);
        layout.panel_top_local = panel_top;
        layout.panel_left_screen = static_cast<int16_t>(frame_x + layout.panel_left_local);
        layout.panel_top_screen = static_cast<int16_t>(frame_y + layout.panel_top_local);
        layout.panel_right_screen = static_cast<int16_t>(layout.panel_left_screen + layout.panel_width - 1U);
        layout.panel_bottom_screen = static_cast<int16_t>(layout.panel_top_screen + layout.panel_height - 1U);
    }
    layout.brightness_left_screen = static_cast<int16_t>(frame_x + layout.brightness_left_local);
    layout.brightness_top_screen = static_cast<int16_t>(frame_y + layout.pill_top_local);
    layout.brightness_right_screen = static_cast<int16_t>(layout.brightness_left_screen + layout.pill_width - 1U);
    layout.brightness_bottom_screen = static_cast<int16_t>(layout.brightness_top_screen + layout.pill_height - 1U);
    layout.volume_left_screen = static_cast<int16_t>(frame_x + layout.volume_left_local);
    layout.volume_top_screen = layout.brightness_top_screen;
    layout.volume_right_screen = static_cast<int16_t>(layout.volume_left_screen + layout.pill_width - 1U);
    layout.volume_bottom_screen = layout.brightness_bottom_screen;

    snapshot.slider_percent = snapshot.selected == VideoControlTarget::Brightness
        ? static_cast<uint8_t>((static_cast<uint16_t>(
            snapshot.brightness_percent > 5U ? snapshot.brightness_percent - 5U : 0U) * 100U) / 95U)
        : snapshot.volume_percent;

    portENTER_CRITICAL(&g_video_controls_mux);
    if (g_video_controls.active) g_video_controls.layout = layout;
    else layout.valid = false;
    portEXIT_CRITICAL(&g_video_controls_mux);
    snapshot.layout = layout;
    if (!layout.valid) return false;
    *out_snapshot = snapshot;
    return true;
}

static esp_err_t benchmark_black_strip_producer(
    void *context,
    uint16_t source_y,
    uint16_t rows,
    uint16_t width,
    uint8_t *dst_rgb565,
    size_t dst_bytes)
{
    (void)context;
    (void)source_y;
    if (dst_rgb565 == nullptr || rows == 0U || width == 0U) return ESP_ERR_INVALID_ARG;
    const size_t required =
        static_cast<size_t>(rows) * static_cast<size_t>(width) * 2U;
    if (required > dst_bytes) return ESP_ERR_INVALID_SIZE;
    memset(dst_rgb565, 0, required);
    return ESP_OK;
}

struct BenchmarkFrameStripContext
{
    const uint8_t *rgb565_be = nullptr;
    uint16_t width = 0U;
    uint16_t height = 0U;
    uint32_t phase_delay_us = 0U;
    bool phase_applied = false;
    // SEEK 完成后的短暂浮层（2 秒），由 Presenter 每帧快照，直接叠加到推流缓冲。
    bool overlay_active = false;
    uint32_t overlay_target_ms = 0U;
    uint32_t overlay_duration_ms = 0U;
    VideoControlsDrawSnapshot controls = {};
};

static uint32_t benchmark_scan_phase_delay_us(uint16_t frame_y)
{
    (void)frame_y;
    return kScanPhaseSafetyUs;
}

static esp_err_t benchmark_frame_strip_producer(
    void *context,
    uint16_t source_y,
    uint16_t rows,
    uint16_t width,
    uint8_t *dst_rgb565,
    size_t dst_bytes)
{
    BenchmarkFrameStripContext *frame = static_cast<BenchmarkFrameStripContext *>(context);
    if (frame == nullptr || frame->rgb565_be == nullptr || dst_rgb565 == nullptr ||
        width == 0U || rows == 0U || width != frame->width ||
        source_y >= frame->height || rows > static_cast<uint16_t>(frame->height - source_y)) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t required =
        static_cast<size_t>(rows) * static_cast<size_t>(width) * 2U;
    if (required > dst_bytes) return ESP_ERR_INVALID_SIZE;

    // display_bounded_spi 在调用首个 producer 前已经完成当前帧的 TE wait；
    // 因此这里只延迟第一次 producer，就能把真正 RAMWR stream 平移到目标 scan phase。
    if (!frame->phase_applied) {
        frame->phase_applied = true;
        if (frame->phase_delay_us > 0U) {
            esp_rom_delay_us(frame->phase_delay_us);
        }
    }

    const uint8_t *src = frame->rgb565_be +
        static_cast<size_t>(source_y) * static_cast<size_t>(width) * 2U;
    memcpy(dst_rgb565, src, required);

    if (frame->overlay_active) {
        benchmark_seek_overlay_draw(
            dst_rgb565, source_y, rows, frame->width, frame->height,
            frame->overlay_target_ms, frame->overlay_duration_ms);
    }
    if (frame->controls.active) {
        benchmark_controls_overlay_draw(dst_rgb565, source_y, rows, frame->width, frame->controls);
    }
    return ESP_OK;
}

static int32_t benchmark_probe_clamp_i32(int64_t value)
{
    if (value > 2147483647LL) return 2147483647;
    if (value < -2147483647LL - 1LL) return -2147483647 - 1;
    return static_cast<int32_t>(value);
}

static void benchmark_probe_record_signed(
    uint32_t &count,
    int64_t &total,
    int32_t &min_value,
    int32_t &max_value,
    int64_t value)
{
    const int32_t sample = benchmark_probe_clamp_i32(value);
    if (count == 0U) {
        min_value = sample;
        max_value = sample;
    } else {
        if (sample < min_value) min_value = sample;
        if (sample > max_value) max_value = sample;
    }
    ++count;
    total += sample;
}

static void benchmark_probe_record_unsigned(
    uint32_t &count,
    uint64_t &total,
    uint32_t &max_value,
    int64_t value)
{
    if (value < 0LL) value = 0LL;
    const uint32_t sample = value > 0xFFFFFFFFLL
        ? 0xFFFFFFFFU : static_cast<uint32_t>(value);
    ++count;
    total += sample;
    if (sample > max_value) max_value = sample;
}

static void benchmark_presenter_mark_exit()
{
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&g_benchmark_ui_mux);
    g_present_running = false;
    if (g_present_task == self) g_present_task = nullptr;
    portEXIT_CRITICAL(&g_benchmark_ui_mux);
}

static void benchmark_presenter_set_error(esp_err_t error)
{
    portENTER_CRITICAL(&g_benchmark_ui_mux);
    if (g_benchmark_ui.display_error == ESP_OK) g_benchmark_ui.display_error = error;
    ++g_benchmark_ui.display_failures;
    portEXIT_CRITICAL(&g_benchmark_ui_mux);
}

static bool benchmark_audio_master_snapshot(AudioVideoClockSnapshot *out_clock)
{
    if (out_clock == nullptr) return false;
    AudioVideoClockSnapshot clock = {};
    if (!audio_service_video_mp3_get_clock(&clock) ||
        !clock.active || clock.sample_rate_hz == 0U ||
        clock.submitted_frames == 0ULL || clock.eof) {
        return false;
    }
    *out_clock = clock;
    return true;
}

// 音频 PCM 时钟的零点就是视频时间轴零点（seek 时音频时钟已对齐到该处）。
// 不能再减 audio_first_pts_ms：seek 后音频探针会记录新的第一帧 PTS，
// 若拿它做基准会把目标时刻整体拉回 0，导致画面被全部判为迟到而冻结。
static int64_t benchmark_audio_master_target_us(uint32_t video_pts_ms, uint32_t audio_first_pts_ms)
{
    (void)audio_first_pts_ms;
    return static_cast<int64_t>(video_pts_ms) * 1000LL;
}

static bool benchmark_presenter_wait_if_paused(bool clock_started, int64_t *clock_base_us)
{
    if (!VideoBenchmark::is_paused()) return !benchmark_presenter_should_stop();

    const int64_t paused_at_us = esp_timer_get_time();
    while (VideoBenchmark::is_paused() && !benchmark_presenter_should_stop()) {
        vTaskDelay(1);
    }
    if (benchmark_presenter_should_stop()) return false;

    if (clock_started && clock_base_us != nullptr) {
        const int64_t paused_us = esp_timer_get_time() - paused_at_us;
        if (paused_us > 0LL) {
            *clock_base_us += paused_us;
            portENTER_CRITICAL(&g_benchmark_ui_mux);
            g_benchmark_ui.clock_base_us = *clock_base_us;
            portEXIT_CRITICAL(&g_benchmark_ui_mux);
        }
    }
    return true;
}

static void benchmark_presenter_task(void *arg)
{
    (void)arg;
    bool bounded_session = false;
    bool clock_started = false;
    uint32_t seek_epoch_seen = 0U;
    bool audio_master_expected = false;
    bool audio_master_logged = false;
    bool audio_start_barrier_released = false;
    // 音频主时钟看门狗状态：用“视频 PTS 进度 vs 音频已提交 PCM 进度”的累计差值判断欠载。
    int64_t audio_master_drift_us = 0LL;
    int64_t audio_master_last_pos_us = 0LL;
    int64_t audio_master_last_pts_us = 0LL;
    bool audio_master_drift_valid = false;
    uint32_t first_pts_ms = 0U;
    uint32_t audio_first_pts_ms = 0U;
    int64_t clock_base_us = 0LL;
    int64_t last_present_done_us = 0LL;
    int64_t last_wake_us = 0LL;
    uint32_t presented_local = 0U;

    ESP_LOGI(TAG, "视频显示任务已启动");

    while (!benchmark_presenter_should_stop()) {
        if (!benchmark_presenter_wait_if_paused(clock_started, &clock_base_us)) break;

        VideoBenchmark::FrameView frame = {};
        const bool got_frame = VideoBenchmark::wait_frame(&frame, kPresenterFrameWaitMs);
        const int64_t acquired_us = esp_timer_get_time();

        // 在 wait_frame 之后检查世代，覆盖 SEEK 恰好发生在 Presenter 阻塞取帧期间的竞态。
        // SEEK 后 PTS 仍沿用 AVI 绝对时间轴，但 Presentation Clock 必须重新锚定。
        const uint32_t epoch = VideoBenchmark::seek_epoch();
        if (epoch != seek_epoch_seen) {
            seek_epoch_seen = epoch;
            clock_started = false;
            audio_master_expected = false;
            audio_master_logged = false;
            audio_master_drift_us = 0LL;
            audio_master_last_pos_us = 0LL;
            audio_master_last_pts_us = 0LL;
            audio_master_drift_valid = false;
            // 新时间轴重新给予首帧显示机会，不能沿用 SEEK 前的累计显示帧数。
            presented_local = 0U;
            last_present_done_us = 0LL;
            first_pts_ms = 0U;
            audio_first_pts_ms = 0U;
            clock_base_us = 0LL;
        }

        portENTER_CRITICAL(&g_benchmark_ui_mux);
        CadenceProbeWindow &probe = g_benchmark_ui.probe;
        ++probe.ticks;
        if (last_wake_us != 0LL) {
            benchmark_probe_record_unsigned(
                probe.tick_gap_samples, probe.tick_gap_us_total, probe.tick_gap_us_max,
                acquired_us - last_wake_us);
        }
        if (!got_frame) ++probe.take_miss;
        portEXIT_CRITICAL(&g_benchmark_ui_mux);
        last_wake_us = acquired_us;

        if (!got_frame) {
            VideoBenchmark::Snapshot snapshot = {};
            if (VideoBenchmark::get_snapshot(&snapshot) &&
                (snapshot.state == VideoBenchmark::State::Eof ||
                 snapshot.state == VideoBenchmark::State::Stopped ||
                 snapshot.state == VideoBenchmark::State::Failed)) {
                break;
            }
            continue;
        }

        if (benchmark_presenter_should_stop() ||
            !benchmark_presenter_wait_if_paused(clock_started, &clock_base_us)) {
            VideoBenchmark::release_frame(frame.slot);
            break;
        }
        // Presenter 可能在 SEEK 开始前已经取到一帧；暂停等待结束后必须再次检查世代。
        if (frame.seek_epoch != VideoBenchmark::seek_epoch()) {
            VideoBenchmark::release_frame(frame.slot);
            continue;
        }

        if (!clock_started) {
            clock_started = true;
            first_pts_ms = frame.pts_ms;
            clock_base_us = acquired_us;

            VideoBenchmark::Snapshot stream_snapshot = {};
            if (VideoBenchmark::get_snapshot(&stream_snapshot) && stream_snapshot.audio_streams != 0U) {
                audio_master_expected = true;
                audio_first_pts_ms = stream_snapshot.audio_first_pts_ms;
            }

            portENTER_CRITICAL(&g_benchmark_ui_mux);
            g_benchmark_ui.clock_started = true;
            g_benchmark_ui.first_pts_ms = first_pts_ms;
            g_benchmark_ui.clock_base_us = clock_base_us;
            portEXIT_CRITICAL(&g_benchmark_ui_mux);
            if (!VideoBenchmark::set_presentation_clock(frame.seek_epoch, first_pts_ms, clock_base_us)) {
                if (frame.seek_epoch != VideoBenchmark::seek_epoch()) {
                    VideoBenchmark::release_frame(frame.slot);
                    continue;
                }
                ESP_LOGW(TAG, "Fallback PTS时钟发布失败；Audio PCM Master不可用时Decode将暂不做PreDecode时钟判断");
            }
        }

        const uint32_t pts_delta_ms = frame.pts_ms >= first_pts_ms
            ? frame.pts_ms - first_pts_ms : 0U;
        const int64_t fallback_target_us =
            clock_base_us + static_cast<int64_t>(pts_delta_ms) * 1000LL;
        const int64_t audio_target_us =
            benchmark_audio_master_target_us(frame.pts_ms, audio_first_pts_ms);

        AudioVideoClockSnapshot audio_clock = {};
        bool use_audio_master =
            audio_master_expected && benchmark_audio_master_snapshot(&audio_clock);

        // 音频欠载会让 position_us 停滞，而视频 PTS 仍在推进；累计落后超过阈值说明音频缺料，
        // 此时切回墙钟节拍，避免整段画面跟着音频主时钟一起慢放（代价是这段内轻微音画偏移）。
        if (use_audio_master) {
            const int64_t pos_us = static_cast<int64_t>(audio_clock.position_us);
            const int64_t pts_us = static_cast<int64_t>(frame.pts_ms) * 1000LL;
            if (audio_master_drift_valid) {
                audio_master_drift_us +=
                    (pts_us - audio_master_last_pts_us) - (pos_us - audio_master_last_pos_us);
                if (audio_master_drift_us > kAudioMasterMaxLagUs) {
                    audio_master_expected = false;
                    use_audio_master = false;
                    ESP_LOGW(TAG, "音频时钟落后视频PTS %lldms，本段改用墙钟节拍",
                        static_cast<long long>(audio_master_drift_us / 1000LL));
                }
            }
            audio_master_last_pos_us = pos_us;
            audio_master_last_pts_us = pts_us;
            audio_master_drift_valid = true;
        }

        const int64_t ready_late_us = use_audio_master
            ? static_cast<int64_t>(audio_clock.position_us) - audio_target_us
            : acquired_us - fallback_target_us;

        if (use_audio_master && !audio_master_logged) {
            audio_master_logged = true;
            portENTER_CRITICAL(&g_benchmark_ui_mux);
            g_benchmark_ui.audio_master_active = true;
            g_benchmark_ui.audio_first_pts_ms = audio_first_pts_ms;
            portEXIT_CRITICAL(&g_benchmark_ui_mux);
        }

        portENTER_CRITICAL(&g_benchmark_ui_mux);
        benchmark_probe_record_signed(
            g_benchmark_ui.probe.ready_samples,
            g_benchmark_ui.probe.ready_late_us_total,
            g_benchmark_ui.probe.ready_late_us_min,
            g_benchmark_ui.probe.ready_late_us_max,
            ready_late_us);
        portEXIT_CRITICAL(&g_benchmark_ui_mux);

        bool pts_waited = false;
        bool epoch_invalidated = false;
        const int64_t pts_wait_started_us = esp_timer_get_time();
        while (!benchmark_presenter_should_stop()) {
            // seek 之后旧的 audio_target_us / fallback_target_us 全部失效，
            // 必须立刻放弃等待，否则 Presenter 会空等数秒并冻住整条管线。
            if (VideoBenchmark::seek_epoch() != seek_epoch_seen) {
                epoch_invalidated = true;
                break;
            }
            if (!benchmark_presenter_wait_if_paused(clock_started, &clock_base_us)) break;
            // 安全阀：任何时钟异常都不允许把 Presenter 钉在这里超过 300ms。
            if (esp_timer_get_time() - pts_wait_started_us > 300000LL) break;
            int64_t wait_us = 0LL;
            if (use_audio_master) {
                AudioVideoClockSnapshot latest_clock = {};
                if (!benchmark_audio_master_snapshot(&latest_clock)) {
                    use_audio_master = false;
                    continue;
                }
                audio_clock = latest_clock;
                wait_us = audio_target_us - kBenchmarkTeArmLeadUs -
                    static_cast<int64_t>(audio_clock.position_us);
            } else {
                wait_us = fallback_target_us - kBenchmarkTeArmLeadUs - esp_timer_get_time();
            }
            if (wait_us <= 0LL) break;
            pts_waited = true;
            uint32_t wait_ms = static_cast<uint32_t>((wait_us + 999LL) / 1000LL);
            if (wait_ms > 10U) wait_ms = 10U;
            TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
            if (wait_ticks == 0) wait_ticks = 1;
            vTaskDelay(wait_ticks);
        }
        if (epoch_invalidated) {
            // 该帧属于 seek 前的旧时间轴，释放后回到外层重新锚定时钟。
            VideoBenchmark::release_frame(frame.slot);
            continue;
        }
        if (pts_waited) {
            portENTER_CRITICAL(&g_benchmark_ui_mux);
            ++g_benchmark_ui.probe.arm_wait;
            portEXIT_CRITICAL(&g_benchmark_ui_mux);
        }
        if (benchmark_presenter_should_stop()) {
            VideoBenchmark::release_frame(frame.slot);
            break;
        }

        int64_t late_us = 0LL;
        if (use_audio_master) {
            AudioVideoClockSnapshot latest_clock = {};
            if (benchmark_audio_master_snapshot(&latest_clock)) {
                audio_clock = latest_clock;
                late_us = static_cast<int64_t>(audio_clock.position_us) - audio_target_us;
            } else {
                use_audio_master = false;
                late_us = esp_timer_get_time() - fallback_target_us;
            }
        } else {
            late_us = esp_timer_get_time() - fallback_target_us;
        }
        if (presented_local > 0U && late_us > kVideoLateDropUs) {
            portENTER_CRITICAL(&g_benchmark_ui_mux);
            ++g_benchmark_ui.dropped;
            portEXIT_CRITICAL(&g_benchmark_ui_mux);
            VideoBenchmark::release_frame(frame.slot);
            continue;
        }

        const uint16_t frame_width = frame.width;
        const uint16_t frame_height = frame.height;
        const size_t expected_frame_bytes =
            static_cast<size_t>(frame_width) * static_cast<size_t>(frame_height) * 2U;
        if (frame_width == 0U || frame_height == 0U ||
            frame_width > VideoBenchmark::kWidth || frame_height > VideoBenchmark::kHeight ||
            frame.bytes != expected_frame_bytes) {
            benchmark_presenter_set_error(ESP_ERR_INVALID_SIZE);
            VideoBenchmark::release_frame(frame.slot);
            VideoBenchmark::stop();
            break;
        }

        const uint16_t frame_x = static_cast<uint16_t>((VideoBenchmark::kWidth - frame_width) / 2U);
        const uint16_t frame_y = static_cast<uint16_t>((VideoBenchmark::kHeight - frame_height) / 2U);
        const uint32_t scan_phase_delay_us = benchmark_scan_phase_delay_us(frame_y);

        if (!bounded_session) {
            const esp_err_t begin_ret = display_launcher_bounded_spi_session_begin();
            if (begin_ret != ESP_OK) {
                benchmark_presenter_set_error(begin_ret);
                VideoBenchmark::release_frame(frame.slot);
                VideoBenchmark::stop();
                break;
            }
            bounded_session = true;

            const esp_err_t clear_ret = display_launcher_bounded_spi_present_stream(
                benchmark_black_strip_producer,
                nullptr,
                0U, 0U,
                VideoBenchmark::kWidth, VideoBenchmark::kHeight,
                true,
                true,
                nullptr);
            if (clear_ret != ESP_OK) {
                benchmark_presenter_set_error(clear_ret);
                VideoBenchmark::release_frame(frame.slot);
                VideoBenchmark::stop();
                break;
            }

            ESP_LOGI(TAG, "视频显示准备完成：%ux%u",
                static_cast<unsigned>(frame_width), static_cast<unsigned>(frame_height));
        }

        if (audio_master_expected && !audio_start_barrier_released) {
            // 首张RGB已经ready，BoundedSPI session/黑场也准备完毕；到这里才允许AudioTask
            // 消费Bridge并提交第一块真实PCM。Presenter 与 AudioTask 同在 Core0，AudioTask/P5 优先；
            // JPEG Decode 独占 Core1，从同一边界并行启动。
            const int64_t release_started_us = esp_timer_get_time();
            if (!audio_service_video_mp3_release_start(true)) {
                ESP_LOGE(TAG, "A/V Start Barrier解除失败；停止本次Video");
                benchmark_presenter_set_error(ESP_FAIL);
                VideoBenchmark::release_frame(frame.slot);
                VideoBenchmark::stop();
                break;
            }
            const int64_t release_wait_us = esp_timer_get_time() - release_started_us;
            audio_start_barrier_released = true;
            ESP_LOGI(TAG,
                "A/V同步启动：Barrier=%lldus master=AudioPCM compensation=0ms video_pts0=%lums audio_pts0=%lums",
                static_cast<long long>(release_wait_us),
                static_cast<unsigned long>(first_pts_ms),
                static_cast<unsigned long>(audio_first_pts_ms));
        }

        BenchmarkFrameStripContext strip_context = {};
        strip_context.rgb565_be = frame.rgb565_be;
        strip_context.width = frame_width;
        strip_context.height = frame_height;
        strip_context.phase_delay_us = scan_phase_delay_us;
        // seek 浮层只在完成后显示 2 秒；每帧取一次快照，过期自动失效。
        strip_context.overlay_active =
            benchmark_seek_overlay_snapshot(&strip_context.overlay_target_ms, &strip_context.overlay_duration_ms);
        (void)benchmark_controls_overlay_snapshot(
            frame_x, frame_y, frame_width, frame_height, &strip_context.controls);

        int64_t present_start_master_late_us = 0LL;
        bool present_start_master_valid = false;
        if (use_audio_master) {
            AudioVideoClockSnapshot clock_at_start = {};
            if (benchmark_audio_master_snapshot(&clock_at_start)) {
                present_start_master_late_us =
                    static_cast<int64_t>(clock_at_start.position_us) - audio_target_us;
                present_start_master_valid = true;
            }
        }

        // SPI 提交前再次检查；若 SEEK 恰好发生在帧准备期间，不提交旧时间轴画面。
        if (frame.seek_epoch != VideoBenchmark::seek_epoch()) {
            VideoBenchmark::release_frame(frame.slot);
            continue;
        }

        const int64_t present_start_us = esp_timer_get_time();
        DisplayBoundedSpiStats stats = {};
        const esp_err_t present_ret = display_launcher_bounded_spi_present_stream(
            benchmark_frame_strip_producer,
            &strip_context,
            frame_x, frame_y,
            frame_width, frame_height,
            true,
            true,
            &stats);
        const int64_t present_done_us = esp_timer_get_time();

        if (present_ret != ESP_OK) {
            benchmark_presenter_set_error(present_ret);
            VideoBenchmark::release_frame(frame.slot);
            VideoBenchmark::stop();
            break;
        }

        int64_t present_start_late_us = present_start_us - fallback_target_us;
        int64_t present_done_late_us = present_done_us - fallback_target_us;
        if (present_start_master_valid) {
            present_start_late_us = present_start_master_late_us;
            AudioVideoClockSnapshot clock_at_done = {};
            if (benchmark_audio_master_snapshot(&clock_at_done)) {
                present_done_late_us =
                    static_cast<int64_t>(clock_at_done.position_us) - audio_target_us;
            } else {
                present_done_late_us = present_start_master_late_us;
            }
        }

        ++presented_local;
        portENTER_CRITICAL(&g_benchmark_ui_mux);
        BenchmarkUiState &ui = g_benchmark_ui;
        benchmark_probe_record_unsigned(
            ui.probe.held_samples, ui.probe.held_wait_us_total, ui.probe.held_wait_us_max,
            present_start_us - acquired_us);
        benchmark_probe_record_signed(
            ui.probe.present_start_samples, ui.probe.present_start_late_us_total,
            ui.probe.present_start_late_us_min, ui.probe.present_start_late_us_max,
            present_start_late_us);
        benchmark_probe_record_signed(
            ui.probe.present_done_samples, ui.probe.present_done_late_us_total,
            ui.probe.present_done_late_us_min, ui.probe.present_done_late_us_max,
            present_done_late_us);
        if (last_present_done_us != 0LL) {
            benchmark_probe_record_unsigned(
                ui.probe.post_idle_samples, ui.probe.post_idle_us_total, ui.probe.post_idle_us_max,
                present_start_us - last_present_done_us);
            benchmark_probe_record_unsigned(
                ui.probe.present_gap_samples, ui.probe.present_gap_us_total, ui.probe.present_gap_us_max,
                present_done_us - last_present_done_us);
        }
        ++ui.presented;
        if (ui.first_present_us == 0LL) ui.first_present_us = present_done_us;
        ui.last_present_us = present_done_us;
        ui.display_us_total += stats.total_us;
        if (stats.total_us > ui.display_us_max) ui.display_us_max = stats.total_us;
        ui.te_us_total += stats.te_wait_us;
        if (stats.te_wait_us > ui.te_us_max) ui.te_us_max = stats.te_wait_us;
        ui.stream_us_total += stats.stream_us;
        if (stats.stream_us > ui.stream_us_max) ui.stream_us_max = stats.stream_us;
        portEXIT_CRITICAL(&g_benchmark_ui_mux);
        last_present_done_us = present_done_us;

        VideoBenchmark::release_frame(frame.slot);

        // SEEK 后音频保持暂停；新位置首帧已真正写入 LCD 后才恢复音频时钟。
        if (frame.seek_epoch == VideoBenchmark::seek_epoch() &&
            VideoBenchmark::seek_audio_resume_pending() &&
            !VideoBenchmark::resume_audio_after_seek()) {
            benchmark_presenter_set_error(ESP_FAIL);
            VideoBenchmark::stop();
            break;
        }
    }

    if (bounded_session) display_launcher_bounded_spi_session_end();
    const BenchmarkUiState ui = benchmark_ui_snapshot();
    ESP_LOGI(TAG, "视频显示结束：显示=%lu 丢帧=%lu 显示错误=%lu",
        static_cast<unsigned long>(ui.presented),
        static_cast<unsigned long>(ui.dropped),
        static_cast<unsigned long>(ui.display_failures));
    benchmark_presenter_mark_exit();
    vTaskDelete(nullptr);
}

static esp_err_t benchmark_presenter_start()
{
    portENTER_CRITICAL(&g_benchmark_ui_mux);
    if (g_present_running) {
        portEXIT_CRITICAL(&g_benchmark_ui_mux);
        return ESP_ERR_INVALID_STATE;
    }
    g_present_stop_requested = false;
    g_present_running = true;
    portEXIT_CRITICAL(&g_benchmark_ui_mux);

    const BaseType_t created = xTaskCreatePinnedToCore(
        benchmark_presenter_task,
        "VideoPresenter",
        kPresenterTaskStack,
        nullptr,
        kPresenterTaskPriority,
        &g_present_task,
        kPresenterTaskCore);
    if (created != pdPASS) {
        portENTER_CRITICAL(&g_benchmark_ui_mux);
        g_present_running = false;
        g_present_task = nullptr;
        portEXIT_CRITICAL(&g_benchmark_ui_mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// 拖动预览基准需要保留到抬手事件消费；此时 current_drag 已经返回 false。
static constexpr int32_t kSeekPreviewStartPx = 18;
static constexpr int64_t kSeekPreviewHoldUs = 250000LL;
static bool g_seek_preview_active = false;
static bool g_seek_preview_base_valid = false;
static uint32_t g_seek_preview_base_ms = 0U;

// 根据触摸起点选择固定的 SEEK 步进，拖动跨过屏幕中线也不会改变速度。
// 上半屏：5px 对应 10 秒（2000ms/px）；下半屏：5px 对应 1 秒（200ms/px）。
static constexpr int32_t kSeekFineMsPerPx = 200;
static constexpr int32_t kSeekCoarseMsPerPx = 2000;

static int32_t benchmark_seek_ms_per_px()
{
    // Router 在抬手后仍保留本次触摸起点；预览和最终提交使用完全相同的判断。
    const int16_t half_height = static_cast<int16_t>(VideoBenchmark::kHeight / 2U);
    const bool from_lower_half = gesture_router_press_started_in_rect(
        0, half_height,
        static_cast<int16_t>(VideoBenchmark::kWidth - 1U),
        static_cast<int16_t>(VideoBenchmark::kHeight - 1U));
    return from_lower_half ? kSeekFineMsPerPx : kSeekCoarseMsPerPx;
}

// 侧车索引 seek：按相对毫秒跳转。依赖 .fvix 索引，无索引时静默忽略。
static void benchmark_seek_by(int32_t delta_ms)
{
    if (!VideoBenchmark::can_seek()) {
        ESP_LOGW(TAG, "Seek不可用：未找到侧车索引(.fvix)");
        return;
    }
    VideoBenchmark::Snapshot snapshot = {};
    if (!VideoBenchmark::get_snapshot(&snapshot)) return;
    if (snapshot.state != VideoBenchmark::State::Running) return;

    // 使用手势开始时记录的同一基准，避免拖动3秒后实际比预览多跳3秒。
    const uint32_t base_ms = g_seek_preview_base_valid
        ? g_seek_preview_base_ms : snapshot.last_pts_ms;
    int64_t target = static_cast<int64_t>(base_ms) + delta_ms;
    if (target < 0) target = 0;
    if (snapshot.duration_ms > 0 && target > static_cast<int64_t>(snapshot.duration_ms)) {
        target = snapshot.duration_ms;
    }
    const esp_err_t ret = VideoBenchmark::seek(static_cast<uint32_t>(target));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Seek失败: delta=%dms ret=%s", delta_ms, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Seek: delta=%dms target=%lums",
            delta_ms, static_cast<unsigned long>(target));
        // 抬手后 seek 已完成，显示 2 秒进度条 + 时间浮层（叠加在视频帧上）。
        benchmark_seek_overlay_show(
            static_cast<uint32_t>(target), snapshot.duration_ms);
    }
}

// 播放页左右滑：上半屏 5px=1 秒，下半屏 5px=10 秒（右滑快进，左滑后退）。
// 抬手后一次执行，避免拖动过程中连续 seek。
// 双击切换暂停/继续：视频 pipeline 与视频 MP3 音频必须一起停/一起走，
// 否则暂停时画面静止但声音继续，恢复后音画不同轴。
static void benchmark_toggle_pause()
{
    const bool pause_now = !VideoBenchmark::is_paused();
    if (pause_now) {
        if (!VideoBenchmark::set_paused(true)) {
            ESP_LOGW(TAG, "双击暂停失败：当前状态不可暂停");
            return;
        }
        (void)audio_service_video_mp3_pause(true);
    } else {
        const bool wait_for_seek_frame = VideoBenchmark::seek_audio_resume_pending();
        if (!wait_for_seek_frame) (void)audio_service_video_mp3_resume(true);
        if (!VideoBenchmark::set_paused(false)) {
            if (!wait_for_seek_frame) (void)audio_service_video_mp3_pause(true);
            ESP_LOGW(TAG, "双击恢复播放失败");
        }
    }
    ESP_LOGI(TAG, "双击：%s", pause_now ? "暂停播放" : "继续播放");
}

static void benchmark_seek_by_swipe()
{
    int16_t dx = 0;
    int16_t dy = 0;
    if (gesture_router_last_displacement(&dx, &dy)) {
        // 使用起点所在半屏的固定步进，与拖动预览共享时间基准和速度。
        benchmark_seek_by(static_cast<int32_t>(dx) * benchmark_seek_ms_per_px());
    }
    g_seek_preview_base_valid = false;
}

// 拖动中的 seek 实时预览：每 tick 把“当前位移 + 起点时间”换成目标时间，刷新同一个浮层。
// 只是显示，不执行 seek；抬手后由 benchmark_seek_by_swipe() 真正跳转并显示 2 秒结果浮层。

static void benchmark_seek_preview_tick()
{
    if (g_page != VideoPage::Benchmark || g_risk_prompt_active ||
        !VideoBenchmark::can_seek()) {
        g_seek_preview_active = false;
        g_seek_preview_base_valid = false;
        return;
    }

    int16_t dx = 0;
    int16_t dy = 0;
    if (!gesture_router_current_drag(&dx, &dy)) {
        // 抬手：保留最后一帧预览 250ms，随后由 seek 结果浮层接管。
        g_seek_preview_active = false;
        return;
    }

    const int32_t adx = dx < 0 ? -dx : dx;
    const int32_t ady = dy < 0 ? -dy : dy;
    // 每个触摸采样都复核方向，确保预览始终满足抬手时Router的提交条件。
    if (adx < kSeekPreviewStartPx || adx * 100 < ady * 135) {
        g_seek_preview_active = false;
        return;
    }
    if (!g_seek_preview_active) {
        VideoBenchmark::Snapshot snapshot = {};
        if (!VideoBenchmark::get_snapshot(&snapshot)) return;
        if (snapshot.state != VideoBenchmark::State::Running) return;
        // 同一轮触摸短暂偏离水平后又回到水平时，继续沿用最初的预览基准。
        if (!g_seek_preview_base_valid) {
            g_seek_preview_base_valid = true;
            g_seek_preview_base_ms = snapshot.last_pts_ms;
        }
        g_seek_preview_active = true;
    }

    VideoBenchmark::Snapshot snapshot = {};
    if (!VideoBenchmark::get_snapshot(&snapshot)) {
        g_seek_preview_active = false;
        return;
    }
    // 预览与抬手提交使用同一个触摸起点和同一个步进，跨越屏幕中线不改变速度。
    int64_t target = static_cast<int64_t>(g_seek_preview_base_ms) +
        static_cast<int64_t>(dx) * benchmark_seek_ms_per_px();
    if (target < 0) target = 0;
    if (snapshot.duration_ms > 0U && target > static_cast<int64_t>(snapshot.duration_ms)) {
        target = snapshot.duration_ms;
    }
    benchmark_seek_overlay_show(
        static_cast<uint32_t>(target), snapshot.duration_ms, kSeekPreviewHoldUs);
}


static void benchmark_controls_apply_brightness(uint8_t level)
{
    if (level < 5U) level = 5U;
    if (level > 100U) level = 100U;
    const esp_err_t ret = screen_lock_simple_set_normal_brightness(level);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "视频亮度预览失败：%u%% ret=%s",
            static_cast<unsigned>(level), esp_err_to_name(ret));
        return;
    }
    portENTER_CRITICAL(&g_video_controls_mux);
    g_video_controls.brightness_percent = level;
    g_video_controls.brightness_dirty = true;
    g_video_controls.until_us = esp_timer_get_time() + kVideoControlsOverlayHoldUs;
    portEXIT_CRITICAL(&g_video_controls_mux);
}

static void benchmark_controls_apply_volume(uint8_t level)
{
    if (level > 100U) level = 100U;
    if (!audio_service_video_mp3_set_volume(level)) {
        ESP_LOGW(TAG, "视频音量请求失败：%u%%", static_cast<unsigned>(level));
        return;
    }
    portENTER_CRITICAL(&g_video_controls_mux);
    g_video_controls.volume_percent = level;
    g_video_controls.until_us = esp_timer_get_time() + kVideoControlsOverlayHoldUs;
    portEXIT_CRITICAL(&g_video_controls_mux);
}

static void benchmark_controls_tick()
{
    if (g_page != VideoPage::Benchmark || g_risk_prompt_active ||
        g_benchmark_ui.terminal_shown) return;

    UiVerticalAdjustSnapshot adjustment = {};
    if (gesture_router_get_vertical_adjust(&adjustment)) {
        VideoControlTarget selected = VideoControlTarget::None;
        uint8_t start_percent = 0U;
        bool can_adjust = false;
        portENTER_CRITICAL(&g_video_controls_mux);
        selected = g_video_controls.selected;
        if (g_video_controls.active && selected != VideoControlTarget::None &&
            (adjustment.active || adjustment.released)) {
            if (!g_video_controls.dragging ||
                g_video_controls.drag_sequence != adjustment.sequence) {
                g_video_controls.drag_sequence = adjustment.sequence;
                g_video_controls.drag_start_percent =
                    selected == VideoControlTarget::Brightness
                        ? g_video_controls.brightness_percent
                        : g_video_controls.volume_percent;
            }
            g_video_controls.dragging = adjustment.active;
            g_video_controls.until_us = esp_timer_get_time() + kVideoControlsOverlayHoldUs;
            start_percent = g_video_controls.drag_start_percent;
            can_adjust = true;
        }
        portEXIT_CRITICAL(&g_video_controls_mux);

        if (can_adjust) {
            const int32_t range = selected == VideoControlTarget::Brightness ? 95 : 100;
            int32_t target = static_cast<int32_t>(start_percent) -
                static_cast<int32_t>(adjustment.delta_y) * range / kVideoControlsDragFullScalePx;
            if (selected == VideoControlTarget::Brightness) {
                if (target < 5) target = 5;
                if (target > 100) target = 100;
                benchmark_controls_apply_brightness(static_cast<uint8_t>(target));
            } else {
                if (target < 0) target = 0;
                if (target > 100) target = 100;
                benchmark_controls_apply_volume(static_cast<uint8_t>(target));
            }
        }

        if (adjustment.released) {
            portENTER_CRITICAL(&g_video_controls_mux);
            if (g_video_controls.drag_sequence == adjustment.sequence) {
                g_video_controls.dragging = false;
                g_video_controls.until_us = esp_timer_get_time() + kVideoControlsOverlayHoldUs;
            }
            portEXIT_CRITICAL(&g_video_controls_mux);
            gesture_router_ack_vertical_adjust_release(adjustment.sequence);
        }
    }

    const bool pointer_down = gesture_router_current_drag(nullptr, nullptr);
    bool expired = false;
    portENTER_CRITICAL(&g_video_controls_mux);
    expired = g_video_controls.active && !g_video_controls.dragging &&
        !pointer_down && esp_timer_get_time() >= g_video_controls.until_us;
    portEXIT_CRITICAL(&g_video_controls_mux);
    if (expired) benchmark_controls_hide();
}

static void benchmark_controls_handle_single_tap()
{
    bool active = false;
    VideoControlTarget selected = VideoControlTarget::None;
    VideoControlsLayout layout = {};
    portENTER_CRITICAL(&g_video_controls_mux);
    active = g_video_controls.active;
    selected = g_video_controls.selected;
    layout = g_video_controls.layout;
    portEXIT_CRITICAL(&g_video_controls_mux);

    if (!active) {
        benchmark_controls_show();
        return;
    }
    if (layout.valid && gesture_router_press_started_in_rect(
            layout.brightness_left_screen, layout.brightness_top_screen,
            layout.brightness_right_screen, layout.brightness_bottom_screen)) {
        benchmark_controls_select(VideoControlTarget::Brightness);
        return;
    }
    if (layout.valid && gesture_router_press_started_in_rect(
            layout.volume_left_screen, layout.volume_top_screen,
            layout.volume_right_screen, layout.volume_bottom_screen)) {
        benchmark_controls_select(VideoControlTarget::Volume);
        return;
    }
    if (layout.valid && selected != VideoControlTarget::None && layout.panel_height >= 64U &&
        gesture_router_press_started_in_rect(
            layout.panel_left_screen, layout.panel_top_screen,
            layout.panel_right_screen, layout.panel_bottom_screen)) {
        benchmark_controls_extend();
        return;
    }
    benchmark_controls_hide();
}

static bool benchmark_controls_consume_double_tap()
{
    bool active = false;
    VideoControlsLayout layout = {};
    portENTER_CRITICAL(&g_video_controls_mux);
    active = g_video_controls.active;
    layout = g_video_controls.layout;
    portEXIT_CRITICAL(&g_video_controls_mux);
    if (!active || !layout.valid) return false;

    if (gesture_router_press_started_in_rect(
            layout.brightness_left_screen, layout.brightness_top_screen,
            layout.brightness_right_screen, layout.brightness_bottom_screen)) {
        benchmark_controls_select(VideoControlTarget::Brightness);
        return true;
    }
    if (gesture_router_press_started_in_rect(
            layout.volume_left_screen, layout.volume_top_screen,
            layout.volume_right_screen, layout.volume_bottom_screen)) {
        benchmark_controls_select(VideoControlTarget::Volume);
        return true;
    }
    return false;
}

static bool benchmark_request_stop(const char *reason)
{
    benchmark_controls_hide();
    gesture_router_set_single_tap_enabled(false);
    benchmark_presenter_request_stop();
    VideoBenchmark::stop();
    const bool audio_stopped = benchmark_stop_avi_audio(reason);
    const bool presenter_stopped = benchmark_presenter_wait_stopped(kPresenterStopWaitMs);
    g_benchmark_ui.cleanup_pending = true;
    const BenchmarkUiState ui = benchmark_ui_snapshot();
    ESP_LOGI(TAG, "MJPEG Benchmark停止请求：reason=%s present=%lu drop=%lu presenter=%s",
        reason != nullptr ? reason : "unknown",
        static_cast<unsigned long>(ui.presented),
        static_cast<unsigned long>(ui.dropped),
        presenter_stopped ? "stopped" : "timeout");
    if (!presenter_stopped) {
        ESP_LOGW(TAG, "Dedicated Presenter在%ums内未退出；暂不恢复Video LVGL以避免与BoundedSPI并发写屏",
            static_cast<unsigned>(kPresenterStopWaitMs));
    } else if (audio_stopped) {
        benchmark_restore_music_after_exclusive(reason);
    }
    return presenter_stopped && audio_stopped;
}

static void benchmark_cleanup_tick()
{
    if (!g_benchmark_ui.cleanup_pending || benchmark_presenter_is_running()) return;
    const esp_err_t ret = VideoBenchmark::cleanup();
    if (ret == ESP_OK) {
        g_benchmark_ui.cleanup_pending = false;
        ESP_LOGI(TAG, "MJPEG Benchmark资源已释放：双RGB565 PSRAM + Extractor queues");
    }
}

static esp_err_t benchmark_wait_cleanup(uint32_t timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000LL;
    TickType_t wait_tick = pdMS_TO_TICKS(10);
    if (wait_tick == 0) wait_tick = 1;

    while (true) {
        if (!benchmark_presenter_is_running()) {
            const esp_err_t ret = VideoBenchmark::cleanup();
            if (ret == ESP_OK) {
                g_benchmark_ui.cleanup_pending = false;
                return ESP_OK;
            }
            if (ret != ESP_ERR_INVALID_STATE) {
                return ret;
            }
        }
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(wait_tick);
    }
}

static bool find_adjacent_video_index(int direction, size_t *out_index)
{
    if (out_index == nullptr || (direction != 1 && direction != -1) ||
        g_selected_index == kInvalidEntryIndex || g_directory.entries == nullptr ||
        g_selected_index >= g_directory.count) return false;

    size_t index = g_selected_index;
    while (true) {
        if (direction > 0) {
            if (index + 1U >= g_directory.count) break;
            ++index;
        } else {
            if (index == 0U) break;
            --index;
        }
        const VideoBrowser::EntryIndex *entry = VideoBrowser::entry_at(&g_directory, index);
        if (entry != nullptr && !VideoBrowser::entry_is_directory(entry)) {
            *out_index = index;
            return true;
        }
    }
    return false;
}

static bool select_video_in_current_directory(int direction, const char *reason)
{
    if (g_current_dir == nullptr || g_selected_path == nullptr || g_scratch_path == nullptr) return false;
    size_t index = kInvalidEntryIndex;
    if (!find_adjacent_video_index(direction, &index)) return false;

    const char *name = VideoBrowser::entry_name(&g_directory, index);
    if (name == nullptr || VideoBrowser::join_child_path(
            g_current_dir, name, g_scratch_path, VideoBrowser::kPathBytes) != ESP_OK) return false;
    snprintf(g_selected_path, VideoBrowser::kPathBytes, "%s", g_scratch_path);
    g_selected_index = index;
    remote_identity_changed();
    ESP_LOGI(TAG, "%s：已选择目录项 %u/%u：%s",
        reason != nullptr ? reason : "视频切换",
        static_cast<unsigned>(index + 1U), static_cast<unsigned>(g_directory.count), name);
    return true;
}

static bool select_next_video_in_current_directory()
{
    return select_video_in_current_directory(+1, "自动下一视频");
}

static void eof_transition_begin(const VideoBenchmark::Snapshot &snapshot)
{
    if (g_eof_transition != EofTransition::None) return;

    const BenchmarkUiState ui = benchmark_ui_snapshot();
    const bool has_next = kAutoPlayNextBringUpEnabled && select_next_video_in_current_directory();
    const char *reason = has_next ? "auto_next" : "eof";
    if (!benchmark_stop_avi_audio(reason)) {
        ESP_LOGE(TAG, "视频自然结束后停止AVI音频失败，回退终止页");
        benchmark_show_terminal(snapshot);
        return;
    }

    g_benchmark_ui.terminal_shown = true;
    g_benchmark_ui.cleanup_pending = true;
    g_eof_transition = has_next ? EofTransition::AutoNext : EofTransition::ReturnBrowser;
    ESP_LOGI(TAG,
        "视频自然播放完成：present=%lu drop=%lu 自动下一视频=%s%s",
        static_cast<unsigned long>(ui.presented), static_cast<unsigned long>(ui.dropped),
        has_next ? "是" : "否",
        has_next ? "；保持视频独占，不恢复Music播放" : "；准备返回视频列表并恢复Music");
}

static void eof_transition_tick()
{
    if (g_eof_transition == EofTransition::None || g_benchmark_ui.cleanup_pending ||
        benchmark_presenter_is_running()) return;

    const EofTransition transition = g_eof_transition;
    g_eof_transition = EofTransition::None;
    if (transition == EofTransition::ReturnBrowser) {
        benchmark_restore_music_after_exclusive("eof");
        show_browser();
        ESP_LOGI(TAG, "视频已播放到当前目录末尾：返回视频列表");
        return;
    }

    // 下一条先走与手动点击完全相同的规格预检。Music播放状态始终保持暂停，
    // 因此不会出现“视频A结束 -> Music短暂出声 -> 视频B再暂停”的听感断点。
    g_page = VideoPage::Browser;
    benchmark_controls_hide();
    gesture_router_set_single_tap_enabled(false);
    gesture_router_set_video_seek_mode(false);
    if (g_timer != nullptr) lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
    set_visible(g_root, false);
    g_fullcanvas_risk_authorized = false;
    g_risk_prompt_active = false;
    if (!start_profile_probe_selected()) {
        ESP_LOGW(TAG, "自动下一视频：规格预检启动失败，已返回视频列表");
        return;
    }
    ESP_LOGI(TAG, "自动下一视频：保持视频独占，正在预检下一AVI规格");
}

static uint32_t benchmark_fps_milli(const BenchmarkUiState &ui)
{
    if (ui.presented < 2U || ui.first_present_us == 0LL ||
        ui.last_present_us <= ui.first_present_us) return 0U;
    const uint64_t elapsed_us = static_cast<uint64_t>(ui.last_present_us - ui.first_present_us);
    const uint64_t intervals = static_cast<uint64_t>(ui.presented - 1U);
    return static_cast<uint32_t>((intervals * 1000000000ULL + elapsed_us / 2ULL) / elapsed_us);
}

static void benchmark_show_terminal(const VideoBenchmark::Snapshot &snapshot)
{
    if (g_benchmark_ui.terminal_shown || g_page != VideoPage::Benchmark || benchmark_presenter_is_running()) return;
    benchmark_controls_hide();
    gesture_router_set_single_tap_enabled(false);
    if (g_timer != nullptr) lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
    set_visible(g_root, true);
    g_benchmark_ui.terminal_shown = true;
    g_benchmark_ui.cleanup_pending = true;
    const char *terminal_reason =
        snapshot.state == VideoBenchmark::State::Eof ? "eof" : "terminal";
    const bool profile_risk_prompt =
        !g_fullcanvas_risk_authorized && snapshot.result == ESP_ERR_NOT_SUPPORTED &&
        snapshot.width == VideoBenchmark::kWidth && snapshot.height == VideoBenchmark::kHeight &&
        snapshot.fps_hint > 20U;
    if (profile_risk_prompt) {
        // 风险探测在 AVI Audio/JPEG 真正启动前就退出；不要把“没有AVI Audio可停”记成错误。
        // 这里只恢复刚才为了探测而暂停的Music，让用户在确认界面期间继续听原歌曲。
        benchmark_restore_music_after_exclusive("profile_risk_prompt");
    } else if (benchmark_stop_avi_audio(terminal_reason)) {
        benchmark_restore_music_after_exclusive(terminal_reason);
    }

    const BenchmarkUiState ui = benchmark_ui_snapshot();
    const uint32_t fps_milli = benchmark_fps_milli(ui);
    const uint32_t decode_avg_us = snapshot.frames_decoded != 0U
        ? static_cast<uint32_t>(snapshot.decode_us_total / snapshot.frames_decoded) : 0U;
    const uint32_t storage_avg_us = snapshot.frames_read != 0U
        ? static_cast<uint32_t>(snapshot.storage_us_total / snapshot.frames_read) : 0U;
    const uint32_t display_avg_us = ui.presented != 0U
        ? static_cast<uint32_t>(ui.display_us_total / ui.presented) : 0U;
    const esp_err_t terminal_error = ui.display_error != ESP_OK ? ui.display_error : snapshot.result;
    const bool pass = terminal_error == ESP_OK && snapshot.state == VideoBenchmark::State::Eof &&
        snapshot.decode_failures == 0U && ui.display_failures == 0U;

    const bool risky_fullcanvas = profile_risk_prompt && terminal_error == ESP_ERR_NOT_SUPPORTED;

    char line1[180] = {};
    char line2[180] = {};
    char line3[200] = {};
    if (risky_fullcanvas) {
        snprintf(line1, sizeof(line1), "460x460 / %uFPS",
            static_cast<unsigned>(snapshot.fps_hint));
        snprintf(line2, sizeof(line2), "高帧率可能掉帧或断音");
        snprintf(line3, sizeof(line3), "建议转为20FPS");
    } else if (pass) {
        snprintf(line1, sizeof(line1), "Native MJPEG Benchmark PASS\n显示 %lu 帧  丢帧 %lu",
            static_cast<unsigned long>(ui.presented),
            static_cast<unsigned long>(ui.dropped));
    } else {
        snprintf(line1, sizeof(line1), "Benchmark结束：%s\n错误：%s",
            VideoBenchmark::state_name(snapshot.state), esp_err_to_name(terminal_error));
    }
    if (!risky_fullcanvas) {
        snprintf(line2, sizeof(line2), "实测 %lu.%03lu fps\nJPEG %lu.%03lu ms avg / %lu.%03lu max",
            static_cast<unsigned long>(fps_milli / 1000U),
            static_cast<unsigned long>(fps_milli % 1000U),
            static_cast<unsigned long>(decode_avg_us / 1000U),
            static_cast<unsigned long>(decode_avg_us % 1000U),
            static_cast<unsigned long>(snapshot.decode_us_max / 1000U),
            static_cast<unsigned long>(snapshot.decode_us_max % 1000U));
        snprintf(line3, sizeof(line3), "TF %lu.%03lu ms avg  Display %lu.%03lu ms avg\npool_skip=%lu gate=%lu次  Dedicated Presenter",
            static_cast<unsigned long>(storage_avg_us / 1000U),
            static_cast<unsigned long>(storage_avg_us % 1000U),
            static_cast<unsigned long>(display_avg_us / 1000U),
            static_cast<unsigned long>(display_avg_us % 1000U),
            static_cast<unsigned long>(snapshot.frames_pool_skipped),
            static_cast<unsigned long>(snapshot.storage_gate_wait_count));
    }

    if (risky_fullcanvas) lv_label_set_text(g_probe_title, "播放提示");
    lv_label_set_text(g_probe_video, line1);
    lv_label_set_text(g_probe_audio, line2);
    lv_label_set_text(g_probe_misc, line3);
    set_visible(g_risk_continue, risky_fullcanvas);
    set_visible(g_risk_cancel, risky_fullcanvas);
    lv_obj_invalidate(g_root);

    if (risky_fullcanvas) {
        ESP_LOGW(TAG,
            "Video播放前风险确认：460x460/%uFPS > 稳定档位20FPS；Music已恢复，等待用户选择继续播放或取消",
            static_cast<unsigned>(snapshot.fps_hint));
    }

    ESP_LOGI(TAG,
        "视频播放结束：状态=%s 结果=%s 显示=%lu 丢帧=%lu 实测=%lu.%03lu帧/秒",
        VideoBenchmark::state_name(snapshot.state), esp_err_to_name(terminal_error),
        static_cast<unsigned long>(ui.presented),
        static_cast<unsigned long>(ui.dropped),
        static_cast<unsigned long>(fps_milli / 1000U),
        static_cast<unsigned long>(fps_milli % 1000U));
}

static bool remote_switch_video(int direction)
{
    size_t target_index = kInvalidEntryIndex;
    if (!find_adjacent_video_index(direction, &target_index)) {
        ESP_LOGI(TAG, "BLE视频切换到达目录边界：direction=%d，保持当前视频", direction);
        return true;
    }
    (void)target_index;

    benchmark_presenter_request_stop();
    VideoBenchmark::stop();
    const bool audio_stopped = benchmark_stop_avi_audio(direction > 0 ? "ble_next" : "ble_previous");
    const bool presenter_stopped = benchmark_presenter_wait_stopped(kPresenterStopWaitMs);
    if (!audio_stopped || !presenter_stopped) {
        ESP_LOGW(TAG, "BLE视频切换失败：direction=%d audio=%u presenter=%u",
            direction, static_cast<unsigned>(audio_stopped), static_cast<unsigned>(presenter_stopped));
        return false;
    }
    g_benchmark_ui.cleanup_pending = true;
    const esp_err_t cleanup_ret = benchmark_wait_cleanup(kDestroyCleanupWaitMs);
    if (cleanup_ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE视频切换资源回收失败：%s", esp_err_to_name(cleanup_ret));
        return false;
    }
    if (!select_video_in_current_directory(
            direction, direction > 0 ? "BLE下一视频" : "BLE上一视频")) {
        ESP_LOGW(TAG, "BLE视频切换：目标项解析失败 direction=%d", direction);
        benchmark_restore_music_after_exclusive("ble_video_select_failed");
        show_browser();
        return false;
    }

    g_page = VideoPage::Browser;
    benchmark_controls_hide();
    gesture_router_set_single_tap_enabled(false);
    gesture_router_set_video_seek_mode(false);
    if (g_timer != nullptr) lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
    set_visible(g_root, false);
    g_fullcanvas_risk_authorized = false;
    g_risk_prompt_active = false;
    if (!start_profile_probe_selected()) {
        ESP_LOGW(TAG, "BLE视频切换：下一条规格预检启动失败");
        return false;
    }
    return true;
}

static void process_remote_command()
{
    VideoRemoteCommand command = VideoRemoteCommand::TogglePlayPause;
    if (!remote_take_pending(&command)) return;

    if (command == VideoRemoteCommand::TogglePlayPause) {
        const bool pause = !VideoBenchmark::is_paused();
        AudioVideoClockSnapshot audio_clock = {};
        const bool has_video_audio =
            audio_service_video_mp3_get_clock(&audio_clock) && audio_clock.active;
        bool ok = false;
        if (pause) {
            ok = VideoBenchmark::set_paused(true);
            if (ok && has_video_audio) ok = audio_service_video_mp3_pause(true);
            if (!ok) (void)VideoBenchmark::set_paused(false);
        } else {
            const bool wait_for_seek_frame = VideoBenchmark::seek_audio_resume_pending();
            ok = wait_for_seek_frame || !has_video_audio || audio_service_video_mp3_resume(true);
            if (ok) ok = VideoBenchmark::set_paused(false);
            if (!ok && has_video_audio && !wait_for_seek_frame) {
                (void)audio_service_video_mp3_pause(true);
            }
        }
        if (ok) {
            remote_playback_changed();
            ESP_LOGI(TAG, "BLE视频控制：%s", pause ? "暂停" : "继续播放");
        } else {
            ESP_LOGW(TAG, "BLE视频控制播放/暂停失败：target=%s", pause ? "pause" : "resume");
        }
        return;
    }

    (void)remote_switch_video(command == VideoRemoteCommand::Next ? +1 : -1);
}

static void benchmark_tick()
{
    benchmark_cleanup_tick();
    eof_transition_tick();
    if (g_page != VideoPage::Benchmark || g_benchmark_ui.terminal_shown) return;

    VideoBenchmark::Snapshot snapshot = {};
    if (!VideoBenchmark::get_snapshot(&snapshot)) return;

    if (!benchmark_presenter_is_running() && snapshot.state == VideoBenchmark::State::Eof) {
        eof_transition_begin(snapshot);
        return;
    }
    if (!benchmark_presenter_is_running() &&
        (snapshot.state == VideoBenchmark::State::Stopped ||
         snapshot.state == VideoBenchmark::State::Failed)) {
        benchmark_show_terminal(snapshot);
    }
}

static void show_launcher()
{
    if (g_root == nullptr || g_page != VideoPage::Browser || g_current_dir == nullptr ||
        strcmp(g_current_dir, VideoBrowser::kRootDirectory) != 0) return;
    const esp_err_t ret = app_launcher_overlay_show(g_root, AppId::Video);
    if (ret != ESP_OK) ESP_LOGE(TAG, "Video圆环Launcher展开失败：%s", esp_err_to_name(ret));
}

static void go_back()
{
    if (g_page == VideoPage::Benchmark) {
        if (g_risk_prompt_active) {
            ESP_LOGI(TAG, "用户关闭高负载视频提示；返回视频列表");
            benchmark_restore_music_after_exclusive("risk_back");
            show_browser();
            lv_obj_invalidate(g_root);
            return;
        }
        if (!benchmark_request_stop("back")) return;
        show_browser();
        lv_obj_invalidate(g_root);
        return;
    }

    // 规格预检状态仍属于视频列表的一次临时操作；返回时先取消预检并恢复列表，
    // 不能按根目录返回处理，否则会误弹出 APP 圆环菜单。
    if (g_profile_probe_pending) {
        VideoProbe::cancel();
        g_profile_probe_pending = false;
        benchmark_restore_music_after_exclusive("profile_back");
        show_browser();
        lv_obj_invalidate(g_root);
        ESP_LOGI(TAG, "用户取消视频规格预检；返回视频列表");
        return;
    }

    // Profile Probe 失败/格式不支持会复用 Browser 页面显示错误状态，并临时隐藏列表。
    // 只要原目录快照仍有效，第一次返回应关闭错误状态并恢复列表。
    if (g_browser_status != nullptr &&
        !lv_obj_has_flag(g_browser_status, LV_OBJ_FLAG_HIDDEN) &&
        g_directory.entries != nullptr && g_directory.count != 0U) {
        show_browser();
        lv_obj_invalidate(g_root);
        ESP_LOGI(TAG, "用户关闭视频状态页；返回视频列表");
        return;
    }

    if (g_current_dir == nullptr || strcmp(g_current_dir, VideoBrowser::kRootDirectory) == 0) {
        show_launcher();
        return;
    }
    if (VideoBrowser::parent_path(g_current_dir, g_scratch_path, VideoBrowser::kPathBytes) != ESP_OK) return;
    snprintf(g_current_dir, VideoBrowser::kPathBytes, "%s", g_scratch_path);
    update_header();
    begin_browser_load();
}

static bool benchmark_start_selected(bool allow_fullcanvas_over20)
{
    const esp_err_t cleanup_ret = VideoBenchmark::cleanup();
    if (cleanup_ret != ESP_OK && cleanup_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Video启动前资源清理失败：%s", esp_err_to_name(cleanup_ret));
        return false;
    }
    if (!benchmark_pause_music_for_exclusive()) {
        show_status("无法暂停当前Music，Video Exclusive未启动", 0xE18A8A);
        return false;
    }

    remote_context_begin();
    benchmark_controls_hide();
    gesture_router_set_single_tap_enabled(false);
    g_page = VideoPage::Benchmark;
    gesture_router_set_video_seek_mode(true);
    if (g_timer != nullptr) lv_timer_set_period(g_timer, kBenchmarkTimerPeriodMs);
    set_visible(g_browser_host, false);
    set_visible(g_probe_host, true);
    set_visible(g_risk_continue, false);
    set_visible(g_risk_cancel, false);
    update_header();
    lv_label_set_text(g_probe_title, basename_of(g_selected_path));
    lv_label_set_text(g_probe_video, "正在准备AVI播放…");
    lv_label_set_text(g_probe_audio, "Dedicated Presenter + Audio PCM Master");
    lv_label_set_text(g_probe_misc, "MJPEG + MP3 / CO5300");

    // 高负载风险已在VideoProbe预检阶段完成；真正进入这里才暂停Music并启动AVI。
    set_visible(g_root, false);
    g_benchmark_ui = {};
    const esp_err_t ret = VideoBenchmark::start(g_selected_path, allow_fullcanvas_over20, benchmark_music_exclusive_active());
    if (ret != ESP_OK) {
        char line[128] = {};
        snprintf(line, sizeof(line), "Benchmark启动失败：%s", esp_err_to_name(ret));
        lv_label_set_text(g_probe_video, line);
        g_benchmark_ui.terminal_shown = true;
        g_benchmark_ui.cleanup_pending = true;
        gesture_router_set_video_seek_mode(false);
        (void)benchmark_stop_avi_audio("benchmark_start_failed");
        benchmark_restore_music_after_exclusive("benchmark_start_failed");
        remote_context_end();
        set_visible(g_root, true);
        if (g_timer != nullptr) lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
        return false;
    }

    benchmark_controls_reset_for_video();
    const esp_err_t presenter_ret = benchmark_presenter_start();
    if (presenter_ret != ESP_OK) {
        VideoBenchmark::stop();
        g_benchmark_ui.display_error = presenter_ret;
        g_benchmark_ui.cleanup_pending = true;
        gesture_router_set_video_seek_mode(false);
        gesture_router_set_single_tap_enabled(false);
        benchmark_controls_hide();
        if (benchmark_stop_avi_audio("presenter_start_failed")) {
            benchmark_restore_music_after_exclusive("presenter_start_failed");
        }
        remote_context_end();
        char line[128] = {};
        snprintf(line, sizeof(line), "Presenter启动失败：%s", esp_err_to_name(presenter_ret));
        lv_label_set_text(g_probe_video, line);
        set_visible(g_root, true);
        if (g_timer != nullptr) lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
        return false;
    }
    g_eof_transition = EofTransition::None;
    return true;
}

static void show_fullcanvas_risk_prompt(const VideoProbe::Snapshot &snapshot)
{
    benchmark_controls_hide();
    gesture_router_set_single_tap_enabled(false);
    g_profile_probe_pending = false;
    g_fullcanvas_risk_authorized = false;
    g_risk_prompt_active = true;
    g_page = VideoPage::Benchmark;
    gesture_router_set_video_seek_mode(false);
    g_benchmark_ui = {};
    // 当前没有VideoBenchmark在运行，防止benchmark_tick读取上一轮terminal snapshot覆盖提示。
    g_benchmark_ui.terminal_shown = true;
    if (g_timer != nullptr) lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
    set_visible(g_root, true);
    set_visible(g_browser_host, false);
    set_visible(g_probe_host, true);
    set_visible(g_risk_cancel, true);
    set_visible(g_risk_continue, true);
    update_header();

    char profile[64] = {};
    snprintf(profile, sizeof(profile), "460x460 / %uFPS", static_cast<unsigned>(snapshot.fps));
    lv_label_set_text(g_probe_title, "播放提示");
    lv_label_set_text(g_probe_video, profile);
    lv_label_set_text(g_probe_audio, "高帧率可能掉帧或断音");
    lv_label_set_text(g_probe_misc, "建议转为20FPS");
    lv_obj_invalidate(g_root);

    ESP_LOGW(TAG,
        "视频播放前风险确认：460x460/%uFPS > 稳定档位20FPS；规格预检只读取信息，Music%s，等待用户选择",
        static_cast<unsigned>(snapshot.fps),
        benchmark_music_exclusive_active() ? "保持暂停" : "保持连续");
}

static void profile_probe_tick()
{
    if (!g_profile_probe_pending || g_page != VideoPage::Browser) return;

    VideoProbe::Snapshot snapshot = {};
    if (!VideoProbe::get_snapshot(&snapshot)) return;
    if (snapshot.state == VideoProbe::State::Running || snapshot.state == VideoProbe::State::Idle) return;

    g_profile_probe_pending = false;
    VideoProbe::cancel();

    if (snapshot.state != VideoProbe::State::Ready || snapshot.result != ESP_OK) {
        char line[96] = {};
        snprintf(line, sizeof(line), "视频规格读取失败：%s", esp_err_to_name(snapshot.result));
        benchmark_restore_music_after_exclusive("profile_probe_failed");
        show_browser();
        show_status(line, 0xE18A8A);
        ESP_LOGW(TAG, "Video播放前Profile Probe失败：%s", esp_err_to_name(snapshot.result));
        return;
    }

    if (!snapshot.has_video || !snapshot.video_is_mjpeg ||
        snapshot.width == 0U || snapshot.height == 0U ||
        snapshot.width > VideoBenchmark::kWidth || snapshot.height > VideoBenchmark::kHeight) {
        benchmark_restore_music_after_exclusive("profile_not_supported");
        show_browser();
        show_status("视频规格不支持", 0xE18A8A);
        ESP_LOGW(TAG,
            "Video Profile不支持：%ux%u video=%s",
            static_cast<unsigned>(snapshot.width), static_cast<unsigned>(snapshot.height),
            snapshot.video_is_mjpeg ? "MJPEG" : "非MJPEG");
        return;
    }
    if (snapshot.has_audio && !snapshot.audio_is_mp3) {
        benchmark_restore_music_after_exclusive("audio_profile_not_supported");
        show_browser();
        show_status("音轨需为MP3", 0xE18A8A);
        ESP_LOGW(TAG, "Video Profile不支持：AVI存在音轨但不是MP3");
        return;
    }

    const bool risky_fullcanvas =
        snapshot.width == VideoBenchmark::kWidth &&
        snapshot.height == VideoBenchmark::kHeight && snapshot.fps > 20U;
    if (risky_fullcanvas) {
        show_fullcanvas_risk_prompt(snapshot);
        return;
    }

    // Profile安全：到这里才进入Video Exclusive，Music只暂停一次，不再发生“探测->恢复->再暂停”的卡顿。
    ESP_LOGI(TAG,
        "Video Profile预检通过：%ux%u/%uFPS%s；现在进入Video Exclusive",
        static_cast<unsigned>(snapshot.width), static_cast<unsigned>(snapshot.height),
        static_cast<unsigned>(snapshot.fps), snapshot.has_audio ? " + MP3" : "");
    const bool chained_from_video = benchmark_music_exclusive_active();
    g_fullcanvas_risk_authorized = true;
    if (!benchmark_start_selected(true)) {
        g_fullcanvas_risk_authorized = false;
        if (chained_from_video) {
            show_browser();
            show_status("下一个视频启动失败", 0xE18A8A);
            ESP_LOGW(TAG, "自动下一视频：启动失败，已返回视频列表");
        }
    }
}

static bool start_profile_probe_selected()
{
    if (g_selected_path == nullptr || g_selected_path[0] == '\0' || g_profile_probe_pending) return false;
    VideoProbe::cancel();
    const esp_err_t ret = VideoProbe::start(g_selected_path);
    if (ret != ESP_OK) {
        char line[96] = {};
        snprintf(line, sizeof(line), "视频规格读取失败：%s", esp_err_to_name(ret));
        benchmark_restore_music_after_exclusive("profile_probe_start_failed");
        show_browser();
        show_status(line, 0xE18A8A);
        ESP_LOGW(TAG, "Video Profile Probe启动失败：%s", esp_err_to_name(ret));
        return false;
    }
    g_profile_probe_pending = true;
    show_status("正在检查视频规格…", 0x8E9AAA);
    if (benchmark_music_exclusive_active()) {
        ESP_LOGI(TAG, "自动下一视频规格预检启动：保持视频独占，Music继续暂停");
    } else {
        ESP_LOGI(TAG, "Video播放前Profile Probe启动：Music保持后台，不暂停AudioTask");
    }
    return true;
}

static void risk_continue_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click() || g_page != VideoPage::Benchmark ||
        !g_risk_prompt_active || g_fullcanvas_risk_authorized) return;

    const bool chained_from_video = benchmark_music_exclusive_active();
    g_risk_prompt_active = false;
    g_fullcanvas_risk_authorized = true;
    ESP_LOGW(TAG, "用户选择继续播放高负载460x460 AVI；保持源FPS，不自动降帧");
    if (!benchmark_start_selected(true)) {
        g_fullcanvas_risk_authorized = false;
        if (chained_from_video) {
            show_browser();
            show_status("下一个视频启动失败", 0xE18A8A);
        }
    }
    gesture_router_reset();
}

static void risk_cancel_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click() || g_page != VideoPage::Benchmark ||
        !g_risk_prompt_active) return;
    ESP_LOGI(TAG, "用户取消高负载460x460 AVI播放；返回视频列表");
    g_fullcanvas_risk_authorized = false;
    g_risk_prompt_active = false;
    benchmark_restore_music_after_exclusive("risk_cancel");
    show_browser();
    gesture_router_reset();
}

static void row_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click() || g_page != VideoPage::Browser ||
        g_profile_probe_pending || g_browser_load.phase != BrowserLoadPhase::Idle) return;
    const uintptr_t encoded = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (encoded == 0U) return;
    const size_t slot = static_cast<size_t>(encoded - 1U);
    const size_t index = g_first_index + slot;
    const VideoBrowser::EntryIndex *entry = VideoBrowser::entry_at(&g_directory, index);
    const char *name = VideoBrowser::entry_name(&g_directory, index);
    if (entry == nullptr || name == nullptr ||
        VideoBrowser::join_child_path(g_current_dir, name, g_scratch_path, VideoBrowser::kPathBytes) != ESP_OK) return;

    if (VideoBrowser::entry_is_directory(entry)) {
        snprintf(g_current_dir, VideoBrowser::kPathBytes, "%s", g_scratch_path);
        update_header();
        begin_browser_load();
        return;
    }

    snprintf(g_selected_path, VideoBrowser::kPathBytes, "%s", g_scratch_path);
    g_selected_index = index;
    g_fullcanvas_risk_authorized = false;
    g_risk_prompt_active = false;
    (void)start_profile_probe_selected();
    gesture_router_reset();
}

static void back_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click()) return;
    go_back();
}

static void timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_root == nullptr || app_manager_foreground() != AppId::Video) return;
    process_remote_command();
    browser_load_tick();
    profile_probe_tick();
    benchmark_tick();
    benchmark_seek_preview_tick();
    benchmark_controls_tick();

    UiGestureAction action = UiGestureAction::None;
    if (!gesture_router_take_action(&action)) {
        // 预览结束但没有形成有效横向手势时，清除旧基准，避免下一次短滑误用上一轮时间。
        if (g_seek_preview_base_valid && !gesture_router_current_drag(nullptr, nullptr)) {
            g_seek_preview_base_valid = false;
        }
        return;
    }
    if (app_launcher_overlay_is_visible()) {
        app_launcher_overlay_hide();
        g_seek_preview_base_valid = false;
        return;
    }
    // 非水平手势不会提交 SEEK，不能把这次拖动基准带到下一次手势。
    if (action != UiGestureAction::SwipeLeft &&
        action != UiGestureAction::SwipeRight) {
        g_seek_preview_base_valid = false;
    }
    switch (action) {
        case UiGestureAction::SwipeLeft:
            // 视频播放中左滑 = 按距离后退
            if (g_page == VideoPage::Benchmark) {
                if (!g_risk_prompt_active) benchmark_seek_by_swipe();
            }
            break;
        case UiGestureAction::SwipeRight:
            // 视频播放中右滑 = 按距离快进；播放页返回列表改由下滑完成
            if (g_page == VideoPage::Benchmark) {
                if (!g_risk_prompt_active) benchmark_seek_by_swipe();
            } else {
                go_back();
            }
            break;
        case UiGestureAction::SwipeUpTrack:
            if (g_page == VideoPage::Browser) shift_window(+1);
            break;
        case UiGestureAction::SwipeDownTrack:
            // 视频播放中下滑 = 返回视频列表；列表页仍用于向上翻页
            if (g_page == VideoPage::Benchmark) {
                if (!g_risk_prompt_active) go_back();
            } else {
                shift_window(-1);
            }
            break;
        case UiGestureAction::PullDownFromTop:
            // 从顶部 42px 内起手的下滑同样是“下滑返回”，避免顶部区域手势失效
            if (g_page == VideoPage::Benchmark && !g_risk_prompt_active) go_back();
            break;
        case UiGestureAction::PullUpFromBottom:
            if (g_page == VideoPage::Browser) show_launcher();
            break;
        case UiGestureAction::DoubleTap:
            // 浮层打开时，双击胶囊优先选中；双击视频其它位置仍切换暂停/继续。
            if (g_page == VideoPage::Benchmark && !g_risk_prompt_active &&
                !g_benchmark_ui.terminal_shown) {
                if (!benchmark_controls_consume_double_tap()) {
                    benchmark_controls_hide();
                    benchmark_toggle_pause();
                }
            }
            break;
        case UiGestureAction::SingleTap:
            // 单击显示亮度/音量胶囊；再次点击胶囊选中后，上下滑动调节。
            if (g_page == VideoPage::Benchmark && !g_risk_prompt_active &&
                !g_benchmark_ui.terminal_shown) {
                benchmark_controls_handle_single_tap();
            }
            break;
        default:
            break;
    }
}

static esp_err_t create_rows()
{
    if (g_browser_host == nullptr) return ESP_ERR_INVALID_STATE;
    for (size_t slot = 0; slot < kVisibleRows; ++slot) {
        RowUi &ui = g_rows[slot];
        ui.row = lv_button_create(g_browser_host);
        if (ui.row == nullptr) return ESP_ERR_NO_MEM;
        ui_common_lock_object(ui.row);
        lv_obj_set_pos(ui.row, 0, static_cast<int32_t>(slot) * (kRowHeight + kRowGap));
        lv_obj_set_size(ui.row, LV_PCT(100), kRowHeight);
        lv_obj_set_style_radius(ui.row, 12, 0);
        lv_obj_set_style_border_width(ui.row, 0, 0);
        lv_obj_set_style_bg_opa(ui.row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(ui.row, 16, 0);
        lv_obj_set_style_pad_right(ui.row, 14, 0);
        lv_obj_add_event_cb(ui.row, row_clicked_cb, LV_EVENT_CLICKED,
            reinterpret_cast<void *>(static_cast<uintptr_t>(slot + 1U)));

        ui.name = make_label(ui.row, "", 0xEEF1F5, LV_TEXT_ALIGN_LEFT);
        if (ui.name == nullptr) return ESP_ERR_NO_MEM;
        lv_obj_set_width(ui.name, 344);
        lv_label_set_long_mode(ui.name, LV_LABEL_LONG_DOT);
        lv_obj_align(ui.name, LV_ALIGN_LEFT_MID, 0, 0);
        ui.kind = make_label(ui.row, "AVI", 0x677385, LV_TEXT_ALIGN_RIGHT);
        if (ui.kind == nullptr) return ESP_ERR_NO_MEM;
        lv_obj_set_width(ui.kind, 54);
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

static esp_err_t video_create()
{
    if (g_root != nullptr) return ESP_OK;
    g_current_dir = static_cast<char *>(heap_caps_calloc(VideoBrowser::kPathBytes, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_scratch_path = static_cast<char *>(heap_caps_calloc(VideoBrowser::kPathBytes, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_selected_path = static_cast<char *>(heap_caps_calloc(VideoBrowser::kPathBytes, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_current_dir == nullptr || g_scratch_path == nullptr || g_selected_path == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    snprintf(g_current_dir, VideoBrowser::kPathBytes, "%s", VideoBrowser::kRootDirectory);

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
    if (g_header_back != nullptr) {
        ui_common_lock_object(g_header_back);
        lv_obj_set_pos(g_header_back, 12, 14);
        lv_obj_set_size(g_header_back, 48, 40);
        lv_obj_set_style_radius(g_header_back, 12, 0);
        lv_obj_set_style_bg_color(g_header_back, lv_color_hex(0x18202B), 0);
        lv_obj_set_style_bg_opa(g_header_back, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(g_header_back, 0, 0);
        lv_obj_set_ext_click_area(g_header_back, 10);
        lv_obj_add_event_cb(g_header_back, back_clicked_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *back = make_label(g_header_back, "<", 0xF0F3F7, LV_TEXT_ALIGN_CENTER);
        if (back != nullptr) lv_obj_center(back);
    }
    g_header_title = make_label(g_root, "视频", 0xF2F4F7, LV_TEXT_ALIGN_CENTER);
    if (g_header_title != nullptr) {
        lv_obj_set_width(g_header_title, 300);
        lv_label_set_long_mode(g_header_title, LV_LABEL_LONG_DOT);
        lv_obj_align(g_header_title, LV_ALIGN_TOP_MID, 0, 20);
    }
    g_header_line = lv_obj_create(g_root);
    if (g_header_line != nullptr) {
        ui_common_lock_object(g_header_line);
        lv_obj_set_size(g_header_line, 390, 1);
        lv_obj_align(g_header_line, LV_ALIGN_TOP_MID, 0, kHeaderHeight - 1);
        lv_obj_set_style_border_width(g_header_line, 0, 0);
        lv_obj_set_style_bg_color(g_header_line, lv_color_hex(0x1C2430), 0);
        lv_obj_set_style_bg_opa(g_header_line, LV_OPA_COVER, 0);
    }

    g_browser_host = lv_obj_create(g_root);
    if (g_browser_host == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    ui_common_lock_object(g_browser_host);
    lv_obj_set_pos(g_browser_host, kContentMargin, kHeaderHeight + 8);
    lv_obj_set_size(g_browser_host, 460 - kContentMargin * 2, 460 - (kHeaderHeight + 8) - kContentMargin);
    lv_obj_set_style_radius(g_browser_host, 0, 0);
    lv_obj_set_style_border_width(g_browser_host, 0, 0);
    lv_obj_set_style_bg_opa(g_browser_host, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(g_browser_host, 0, 0);
    lv_obj_remove_flag(g_browser_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_browser_host, LV_SCROLLBAR_MODE_OFF);
    const esp_err_t row_ret = create_rows();
    if (row_ret != ESP_OK) return cleanup_create_failure(row_ret);

    g_probe_host = lv_obj_create(g_root);
    if (g_probe_host == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    ui_common_lock_object(g_probe_host);
    lv_obj_set_pos(g_probe_host, 18, kHeaderHeight + 12);
    lv_obj_set_size(g_probe_host, 424, 360);
    lv_obj_set_style_radius(g_probe_host, 14, 0);
    lv_obj_set_style_border_width(g_probe_host, 1, 0);
    lv_obj_set_style_border_color(g_probe_host, lv_color_hex(0x263242), 0);
    lv_obj_set_style_bg_color(g_probe_host, lv_color_hex(0x10151C), 0);
    lv_obj_set_style_bg_opa(g_probe_host, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_probe_host, 18, 0);
    lv_obj_remove_flag(g_probe_host, LV_OBJ_FLAG_SCROLLABLE);
    g_probe_title = make_label(g_probe_host, "", 0xEEF1F5, LV_TEXT_ALIGN_LEFT);
    g_probe_video = make_label(g_probe_host, "", 0xBFC9D6, LV_TEXT_ALIGN_LEFT);
    g_probe_audio = make_label(g_probe_host, "", 0xBFC9D6, LV_TEXT_ALIGN_LEFT);
    g_probe_misc = make_label(g_probe_host, "", 0x8995A5, LV_TEXT_ALIGN_LEFT);
    if (g_probe_title == nullptr || g_probe_video == nullptr || g_probe_audio == nullptr || g_probe_misc == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    lv_obj_set_width(g_probe_title, 388); lv_label_set_long_mode(g_probe_title, LV_LABEL_LONG_DOT); lv_obj_align(g_probe_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_width(g_probe_video, 388); lv_label_set_long_mode(g_probe_video, LV_LABEL_LONG_WRAP); lv_obj_align(g_probe_video, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_obj_set_width(g_probe_audio, 388); lv_label_set_long_mode(g_probe_audio, LV_LABEL_LONG_WRAP); lv_obj_align(g_probe_audio, LV_ALIGN_TOP_LEFT, 0, 150);
    lv_obj_set_width(g_probe_misc, 388); lv_label_set_long_mode(g_probe_misc, LV_LABEL_LONG_WRAP); lv_obj_align(g_probe_misc, LV_ALIGN_TOP_LEFT, 0, 226);

    g_risk_cancel = lv_button_create(g_probe_host);
    g_risk_continue = lv_button_create(g_probe_host);
    if (g_risk_cancel == nullptr || g_risk_continue == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    ui_common_lock_object(g_risk_cancel);
    ui_common_lock_object(g_risk_continue);
    lv_obj_set_size(g_risk_cancel, 174, 46);
    lv_obj_set_size(g_risk_continue, 174, 46);
    lv_obj_align(g_risk_cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_align(g_risk_continue, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(g_risk_cancel, 12, 0);
    lv_obj_set_style_radius(g_risk_continue, 12, 0);
    lv_obj_set_style_border_width(g_risk_cancel, 0, 0);
    lv_obj_set_style_border_width(g_risk_continue, 0, 0);
    lv_obj_set_style_bg_color(g_risk_cancel, lv_color_hex(0x232B36), 0);
    lv_obj_set_style_bg_color(g_risk_continue, lv_color_hex(0x7B2D2D), 0);
    lv_obj_set_style_bg_opa(g_risk_cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(g_risk_continue, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(g_risk_cancel, risk_cancel_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g_risk_continue, risk_continue_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel_label = make_label(g_risk_cancel, "取消", 0xE8EDF4, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *continue_label = make_label(g_risk_continue, "继续播放", 0xFFFFFF, LV_TEXT_ALIGN_CENTER);
    if (cancel_label == nullptr || continue_label == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    lv_obj_center(cancel_label);
    lv_obj_center(continue_label);
    set_visible(g_risk_cancel, false);
    set_visible(g_risk_continue, false);
    set_visible(g_probe_host, false);

    g_timer = lv_timer_create(timer_cb, kBrowserTimerPeriodMs, nullptr);
    if (g_timer == nullptr) return cleanup_create_failure(ESP_ERR_NO_MEM);
    lv_timer_pause(g_timer);
    set_visible(g_root, false);
    ESP_LOGI(TAG, "Video create完成：AVI Virtual Browser 5-Row + MJPEG Dedicated Presenter Benchmark");
    return ESP_OK;
}

static esp_err_t video_enter()
{
    if (g_root == nullptr) return ESP_ERR_INVALID_STATE;
    g_page = VideoPage::Browser;
    benchmark_controls_hide();
    gesture_router_set_single_tap_enabled(false);
    gesture_router_set_video_seek_mode(false);
    snprintf(g_current_dir, VideoBrowser::kPathBytes, "%s", VideoBrowser::kRootDirectory);
    set_visible(g_root, true);
    set_visible(g_browser_host, true);
    set_visible(g_probe_host, false);
    update_header();
    gesture_router_reset();
    if (g_timer != nullptr) {
        lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
        lv_timer_resume(g_timer);
    }
    begin_browser_load();
    ESP_LOGI(TAG, "Video进入Foreground：/video AVI Browser已显示；目录页Music保持后台，启动AVI时暂停Music并由AudioTask接管AVI MP3");
    return ESP_OK;
}

static esp_err_t video_leave(AppRunState next_state)
{
    (void)next_state;
    benchmark_controls_hide();
    gesture_router_set_single_tap_enabled(false);
    remote_context_end();
    if (g_profile_probe_pending) {
        VideoProbe::cancel();
        g_profile_probe_pending = false;
    }
    if (g_page == VideoPage::Benchmark && !g_risk_prompt_active) {
        if (!benchmark_request_stop("leave")) return ESP_ERR_TIMEOUT;
    }
    if (g_benchmark_ui.cleanup_pending) {
        const esp_err_t cleanup_ret = benchmark_wait_cleanup(kDestroyCleanupWaitMs);
        if (cleanup_ret != ESP_OK) {
            // AppManager 会保持 Video 为当前前台；Presenter 已停时恢复 Browser，避免留在隐藏的视频画面。
            if (!benchmark_presenter_is_running()) show_browser();
            ESP_LOGW(TAG, "Video离开延后：Benchmark资源尚未安全回收 ret=%s",
                esp_err_to_name(cleanup_ret));
            return cleanup_ret;
        }
    }
    if (!benchmark_presenter_is_running()) {
        benchmark_restore_music_after_exclusive("leave");
        if (benchmark_music_exclusive_active()) {
            // Queue pressure can reject admission; keep a usable page for retry.
            show_browser();
            return ESP_ERR_TIMEOUT;
        }
    }
    g_page = VideoPage::Browser;
    g_risk_prompt_active = false;
    gesture_router_set_video_seek_mode(false);
    g_eof_transition = EofTransition::None;
    cancel_browser_load();
    if (g_timer != nullptr) lv_timer_pause(g_timer);
    gesture_router_reset();
    app_launcher_overlay_hide();
    set_visible(g_root, false);
    benchmark_controls_commit_brightness();
    ESP_LOGI(TAG, "Video离开Foreground：MJPEG Benchmark/目录协作任务已停止，Video Exclusive Music状态已恢复");
    return ESP_OK;
}

static void video_destroy()
{
    benchmark_controls_hide();
    gesture_router_set_single_tap_enabled(false);
    gesture_router_set_video_seek_mode(false);
    remote_context_end();
    VideoProbe::cancel();
    g_profile_probe_pending = false;
    if ((g_page == VideoPage::Benchmark && !g_risk_prompt_active) || g_benchmark_ui.cleanup_pending) {
        (void)benchmark_request_stop("destroy");
        const esp_err_t cleanup_ret = benchmark_wait_cleanup(kDestroyCleanupWaitMs);
        if (cleanup_ret != ESP_OK) {
            // 正常 AppManager 路径会在 leave() 阶段就拦住未完成的回收；这里只是防御兜底。
            ESP_LOGW(TAG, "Video destroy：Benchmark资源回收异常 ret=%s",
                esp_err_to_name(cleanup_ret));
        }
    }
    cancel_browser_load();
    VideoBrowser::release_directory(&g_directory);
    if (g_timer != nullptr) { lv_timer_delete(g_timer); g_timer = nullptr; }
    app_launcher_overlay_destroy();
    g_header_back = nullptr;
    g_header_title = nullptr;
    g_header_line = nullptr;
    g_browser_host = nullptr;
    g_browser_status = nullptr;
    g_browser_position = nullptr;
    for (RowUi &ui : g_rows) ui = {};
    g_probe_host = nullptr;
    g_probe_title = nullptr;
    g_probe_video = nullptr;
    g_probe_audio = nullptr;
    g_probe_misc = nullptr;
    g_risk_continue = nullptr;
    g_risk_cancel = nullptr;
    if (g_root != nullptr) { lv_obj_delete(g_root); g_root = nullptr; }
    if (g_current_dir != nullptr) heap_caps_free(g_current_dir);
    if (g_scratch_path != nullptr) heap_caps_free(g_scratch_path);
    if (g_selected_path != nullptr) heap_caps_free(g_selected_path);
    g_current_dir = nullptr;
    g_scratch_path = nullptr;
    g_selected_path = nullptr;
    g_selected_index = kInvalidEntryIndex;
    g_eof_transition = EofTransition::None;
    g_page = VideoPage::Browser;
    g_risk_prompt_active = false;
    if (!benchmark_presenter_is_running()) benchmark_restore_music_after_exclusive("destroy");
    benchmark_controls_commit_brightness();
    g_benchmark_ui = {};
    ESP_LOGI(TAG, "Video destroy完成：LVGL/目录索引/路径PSRAM已释放；Benchmark已请求回收，Exclusive Music状态已处理");
}

} // namespace

bool video_app_remote_control_active()
{
    bool active = false;
    portENTER_CRITICAL(&g_remote_mux);
    active = g_remote_context_active;
    portEXIT_CRITICAL(&g_remote_mux);
    return active;
}

bool video_app_remote_submit(VideoRemoteCommand command)
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

bool video_app_get_remote_snapshot(VideoRemoteSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    VideoRemoteSnapshot snapshot = {};
    portENTER_CRITICAL(&g_remote_mux);
    snapshot.active = g_remote_context_active;
    snapshot.item_index = g_remote_item_index;
    snapshot.playback_revision = g_remote_playback_revision;
    snapshot.metadata_revision = g_remote_metadata_revision;
    memcpy(snapshot.title, g_remote_title, sizeof(snapshot.title));
    portEXIT_CRITICAL(&g_remote_mux);
    if (!snapshot.active) {
        *out_snapshot = snapshot;
        return true;
    }

    VideoBenchmark::Snapshot video = {};
    if (VideoBenchmark::get_snapshot(&video)) {
        snapshot.paused = video.paused;
        snapshot.playing = video.state == VideoBenchmark::State::Running && !video.paused;
        snapshot.duration_ms = video.duration_ms;
    }
    AudioVideoClockSnapshot clock = {};
    if (audio_service_video_mp3_get_clock(&clock) && clock.active) {
        snapshot.position_ms = static_cast<uint32_t>(clock.position_us / 1000ULL);
        snapshot.paused = clock.paused || snapshot.paused;
        snapshot.playing = !snapshot.paused && !clock.eof;
    }
    *out_snapshot = snapshot;
    return true;
}

esp_err_t video_app_register()
{
    const esp_err_t probe_ret = VideoProbe::init();
    if (probe_ret != ESP_OK) {
        ESP_LOGE(TAG, "Video Probe初始化失败：%s", esp_err_to_name(probe_ret));
        return probe_ret;
    }
    AppDescriptor descriptor = {};
    descriptor.id = AppId::Video;
    descriptor.name = "视频";
    descriptor.supports_background = false;
    descriptor.lifecycle.create = video_create;
    descriptor.lifecycle.enter = video_enter;
    descriptor.lifecycle.leave = video_leave;
    descriptor.lifecycle.destroy = video_destroy;
    const esp_err_t ret = app_manager_register(descriptor);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Video APP已注册：AVI Browser + Dedicated Presenter + Audio Master + 自动下一视频核心");
    }
    return ret;
}
