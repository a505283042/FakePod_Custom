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
static constexpr size_t kGestureStepRows = 4U;
static constexpr uint32_t kFlacSafePercent = 90U;
static constexpr size_t kScanBatchNoFlac = 8U;
static constexpr size_t kScanBatchWithFlac = 1U;
static constexpr uint32_t kWaitLogIntervalMs = 1000U;
static constexpr int64_t kVideoLateDropUs = 50000LL;
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
static bool g_fullcanvas_risk_authorized = false;
static bool g_profile_probe_pending = false;
static bool g_risk_prompt_active = false;
static size_t g_selected_index = kInvalidEntryIndex;
static EofTransition g_eof_transition = EofTransition::None;

static bool start_profile_probe_selected();
static void benchmark_show_terminal(const VideoBenchmark::Snapshot &snapshot);

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

static bool benchmark_pause_music_for_exclusive()
{
    if (g_music_paused_for_benchmark) return true;

    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        ESP_LOGI(TAG, "Video Exclusive：AudioTask未就绪，无活动Music需要暂停");
        return true;
    }
    if (snapshot.state != AudioPlaybackState::Playing) {
        ESP_LOGI(TAG, "Video Exclusive：Music当前非Playing(state=%u)，保持原状态",
            static_cast<unsigned>(snapshot.state));
        return true;
    }

    if (!audio_service_pause(true)) {
        ESP_LOGE(TAG, "Video Exclusive：暂停Music失败，取消AVI启动");
        return false;
    }
    g_music_paused_for_benchmark = true;
    ESP_LOGI(TAG, "Video Exclusive：Music已暂停；AVI独占TF/Decode/Presenter性能窗口");
    return true;
}

static void benchmark_restore_music_after_exclusive(const char *reason)
{
    if (!g_music_paused_for_benchmark) return;
    if (!audio_service_resume(false)) {
        ESP_LOGW(TAG, "Video Exclusive：恢复Music请求失败 reason=%s；保留暂停标记等待后续生命周期重试",
            reason != nullptr ? reason : "unknown");
        return;
    }
    g_music_paused_for_benchmark = false;
    ESP_LOGI(TAG, "Video Exclusive：已请求恢复Music reason=%s",
        reason != nullptr ? reason : "unknown");
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
    const size_t max_first = g_directory.count > kVisibleRows ? g_directory.count - kVisibleRows : 0U;
    if (g_first_index > max_first) g_first_index = max_first;

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
    const size_t max_first = g_directory.count - kVisibleRows;
    if (direction > 0) {
        const size_t candidate = old + kGestureStepRows;
        g_first_index = candidate < max_first ? candidate : max_first;
    } else {
        g_first_index = old > kGestureStepRows ? old - kGestureStepRows : 0U;
    }
    if (old == g_first_index) return;
    update_rows();
    ESP_LOGI(TAG, "Video虚拟目录：first=%u total=%u direction=%s",
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
    g_page = VideoPage::Browser;
    g_fullcanvas_risk_authorized = false;
    g_risk_prompt_active = false;
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

static int64_t benchmark_audio_master_target_us(uint32_t video_pts_ms, uint32_t audio_first_pts_ms)
{
    return (static_cast<int64_t>(video_pts_ms) -
            static_cast<int64_t>(audio_first_pts_ms)) * 1000LL;
}

static void benchmark_presenter_task(void *arg)
{
    (void)arg;
    bool bounded_session = false;
    bool clock_started = false;
    bool audio_master_expected = false;
    bool audio_master_logged = false;
    bool audio_start_barrier_released = false;
    uint32_t first_pts_ms = 0U;
    uint32_t audio_first_pts_ms = 0U;
    int64_t clock_base_us = 0LL;
    int64_t last_present_done_us = 0LL;
    int64_t last_wake_us = 0LL;
    uint32_t presented_local = 0U;

    ESP_LOGI(TAG, "视频显示任务已启动");

    while (!benchmark_presenter_should_stop()) {
        VideoBenchmark::FrameView frame = {};
        const bool got_frame = VideoBenchmark::wait_frame(&frame, kPresenterFrameWaitMs);
        const int64_t acquired_us = esp_timer_get_time();

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

        if (benchmark_presenter_should_stop()) {
            VideoBenchmark::release_frame(frame.slot);
            break;
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
            if (!VideoBenchmark::set_presentation_clock(first_pts_ms, clock_base_us)) {
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
        while (!benchmark_presenter_should_stop()) {
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

static bool benchmark_request_stop(const char *reason)
{
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

static bool select_next_video_in_current_directory()
{
    if (g_selected_index == kInvalidEntryIndex || g_directory.entries == nullptr ||
        g_selected_index >= g_directory.count || g_current_dir == nullptr ||
        g_selected_path == nullptr || g_scratch_path == nullptr) return false;

    for (size_t index = g_selected_index + 1U; index < g_directory.count; ++index) {
        const VideoBrowser::EntryIndex *entry = VideoBrowser::entry_at(&g_directory, index);
        const char *name = VideoBrowser::entry_name(&g_directory, index);
        if (entry == nullptr || name == nullptr || VideoBrowser::entry_is_directory(entry)) continue;
        if (VideoBrowser::join_child_path(
                g_current_dir, name, g_scratch_path, VideoBrowser::kPathBytes) != ESP_OK) continue;
        snprintf(g_selected_path, VideoBrowser::kPathBytes, "%s", g_scratch_path);
        g_selected_index = index;
        ESP_LOGI(TAG, "自动下一视频：已选择目录项 %u/%u：%s",
            static_cast<unsigned>(index + 1U), static_cast<unsigned>(g_directory.count), name);
        return true;
    }
    return false;
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

    g_page = VideoPage::Benchmark;
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
    const esp_err_t ret = VideoBenchmark::start(g_selected_path, allow_fullcanvas_over20);
    if (ret != ESP_OK) {
        char line[128] = {};
        snprintf(line, sizeof(line), "Benchmark启动失败：%s", esp_err_to_name(ret));
        lv_label_set_text(g_probe_video, line);
        g_benchmark_ui.terminal_shown = true;
        g_benchmark_ui.cleanup_pending = true;
        (void)benchmark_stop_avi_audio("benchmark_start_failed");
        benchmark_restore_music_after_exclusive("benchmark_start_failed");
        set_visible(g_root, true);
        if (g_timer != nullptr) lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
        return false;
    }

    const esp_err_t presenter_ret = benchmark_presenter_start();
    if (presenter_ret != ESP_OK) {
        VideoBenchmark::stop();
        g_benchmark_ui.display_error = presenter_ret;
        g_benchmark_ui.cleanup_pending = true;
        if (benchmark_stop_avi_audio("presenter_start_failed")) {
            benchmark_restore_music_after_exclusive("presenter_start_failed");
        }
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
    g_profile_probe_pending = false;
    g_fullcanvas_risk_authorized = false;
    g_risk_prompt_active = true;
    g_page = VideoPage::Benchmark;
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
        g_music_paused_for_benchmark ? "保持暂停" : "保持连续");
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
    const bool chained_from_video = g_music_paused_for_benchmark;
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
    if (g_music_paused_for_benchmark) {
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

    const bool chained_from_video = g_music_paused_for_benchmark;
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
    browser_load_tick();
    profile_probe_tick();
    benchmark_tick();

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
            if (g_page == VideoPage::Browser) shift_window(+1);
            break;
        case UiGestureAction::SwipeDownTrack:
            if (g_page == VideoPage::Browser) shift_window(-1);
            break;
        case UiGestureAction::PullUpFromBottom:
            if (g_page == VideoPage::Browser) show_launcher();
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
    g_page = VideoPage::Browser;
    g_risk_prompt_active = false;
    g_eof_transition = EofTransition::None;
    cancel_browser_load();
    if (g_timer != nullptr) lv_timer_pause(g_timer);
    gesture_router_reset();
    app_launcher_overlay_hide();
    set_visible(g_root, false);
    if (!benchmark_presenter_is_running()) benchmark_restore_music_after_exclusive("leave");
    ESP_LOGI(TAG, "Video离开Foreground：MJPEG Benchmark/目录协作任务已停止，Video Exclusive Music状态已恢复");
    return ESP_OK;
}

static void video_destroy()
{
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
    g_benchmark_ui = {};
    ESP_LOGI(TAG, "Video destroy完成：LVGL/目录索引/路径PSRAM已释放；Benchmark已请求回收，Exclusive Music状态已处理");
}

} // namespace

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
        ESP_LOGI(TAG, "Video APP已注册：AVI Browser + Dedicated Presenter + Audio Master + 自动下一视频核心");
    }
    return ret;
}
