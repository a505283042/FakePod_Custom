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
static constexpr int64_t kBenchmarkReportIntervalUs = 5000000LL;
static constexpr uint32_t kDestroyCleanupWaitMs = 300U;
static constexpr uint32_t kBrowserTimerPeriodMs = 20U;
static constexpr uint32_t kBenchmarkTimerPeriodMs = 5U;
static constexpr uint32_t kPresenterTaskStack = 4096U;
static constexpr UBaseType_t kPresenterTaskPriority = 2U;
static constexpr BaseType_t kPresenterTaskCore = 1;
static constexpr uint32_t kPresenterFrameWaitMs = 20U;
static constexpr uint32_t kPresenterStopWaitMs = 250U;

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
    uint32_t audio_master_revision = 0U;
    uint32_t audio_first_pts_ms = 0U;
    bool terminal_shown = false;
    bool cleanup_pending = false;
    uint32_t first_pts_ms = 0U;
    int64_t clock_base_us = 0LL;
    int64_t first_present_us = 0LL;
    int64_t last_present_us = 0LL;
    int64_t last_report_us = 0LL;
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

    int64_t held_acquired_us = 0LL;
    int64_t probe_last_tick_us = 0LL;
    int64_t probe_last_present_done_us = 0LL;
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
    if (g_current_dir != nullptr) heap_caps_free(g_current_dir);
    if (g_scratch_path != nullptr) heap_caps_free(g_scratch_path);
    if (g_selected_path != nullptr) heap_caps_free(g_selected_path);
    g_current_dir = nullptr;
    g_scratch_path = nullptr;
    g_selected_path = nullptr;
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
        lv_label_set_text(g_header_title, "MJPEG Benchmark");
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
        show_status("/VIDEO 中没有 AVI", 0x7F8A99);
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
        g_current_dir == nullptr || app_manager_foreground() != AppId::Video) return;

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
            show_status(ret == ESP_ERR_NOT_FOUND ? "请在TF卡根目录创建 VIDEO 文件夹" : "视频目录打开失败", 0xE18A8A);
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
    if (g_timer != nullptr) lv_timer_set_period(g_timer, kBrowserTimerPeriodMs);
    set_visible(g_root, true);
    set_visible(g_browser_host, true);
    set_visible(g_probe_host, false);
    update_header();
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
    uint32_t first_pts_ms = 0U;
    uint32_t audio_first_pts_ms = 0U;
    int64_t clock_base_us = 0LL;
    int64_t last_present_done_us = 0LL;
    int64_t last_wake_us = 0LL;
    uint32_t presented_local = 0U;

    ESP_LOGI(TAG,
        "Dedicated Presenter启动：Core%d/P%u stack=%uB frame_wait=%ums；阻塞等待RGB ready queue，LVGL不再提交视频帧",
        static_cast<int>(kPresenterTaskCore), static_cast<unsigned>(kPresenterTaskPriority),
        static_cast<unsigned>(kPresenterTaskStack), static_cast<unsigned>(kPresenterFrameWaitMs));

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
            g_benchmark_ui.last_report_us = clock_base_us;
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
            g_benchmark_ui.audio_master_revision = audio_clock.revision;
            g_benchmark_ui.audio_first_pts_ms = audio_first_pts_ms;
            portEXIT_CRITICAL(&g_benchmark_ui_mux);
            ESP_LOGI(TAG,
                "Audio Master A/V Sync已接管：clock_rev=%lu audio_pts0=%lums video_pts0=%lums av0_delta=%ldms pcm=%lluus；Presenter等待/late/drop以AudioTask submitted PCM为准",
                static_cast<unsigned long>(audio_clock.revision),
                static_cast<unsigned long>(audio_first_pts_ms),
                static_cast<unsigned long>(first_pts_ms),
                static_cast<long>(static_cast<int32_t>(audio_first_pts_ms) - static_cast<int32_t>(first_pts_ms)),
                static_cast<unsigned long long>(audio_clock.position_us));
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

            ESP_LOGI(TAG,
                "MJPEG Benchmark进入Dedicated Presenter局部GRAM：video=%ux%u window=(%u,%u) canvas=460x460；TE-aware arm=14ms；Minimal Tear Guard=ON phase=%uus@Y=%u；LVGL Present=OFF",
                static_cast<unsigned>(frame_width), static_cast<unsigned>(frame_height),
                static_cast<unsigned>(frame_x), static_cast<unsigned>(frame_y),
                static_cast<unsigned>(scan_phase_delay_us),
                static_cast<unsigned>(frame_y));
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
    ESP_LOGI(TAG,
        "Dedicated Presenter结束：present=%lu drop=%lu display_fail=%lu",
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

static uint32_t benchmark_fps_milli(const BenchmarkUiState &ui)
{
    if (ui.presented < 2U || ui.first_present_us == 0LL ||
        ui.last_present_us <= ui.first_present_us) return 0U;
    const uint64_t elapsed_us = static_cast<uint64_t>(ui.last_present_us - ui.first_present_us);
    const uint64_t intervals = static_cast<uint64_t>(ui.presented - 1U);
    return static_cast<uint32_t>((intervals * 1000000000ULL + elapsed_us / 2ULL) / elapsed_us);
}

static bool benchmark_take_report_snapshot(int64_t now_us, BenchmarkUiState *out_ui)
{
    if (out_ui == nullptr) return false;
    bool ready = false;
    portENTER_CRITICAL(&g_benchmark_ui_mux);
    if (g_benchmark_ui.presented != 0U &&
        (g_benchmark_ui.last_report_us == 0LL ||
         now_us - g_benchmark_ui.last_report_us >= kBenchmarkReportIntervalUs)) {
        g_benchmark_ui.last_report_us = now_us;
        *out_ui = g_benchmark_ui;
        g_benchmark_ui.probe = {};
        ready = true;
    }
    portEXIT_CRITICAL(&g_benchmark_ui_mux);
    return ready;
}

static void benchmark_log_realtime(const VideoBenchmark::Snapshot &snapshot, int64_t now_us)
{
    BenchmarkUiState ui = {};
    if (!benchmark_take_report_snapshot(now_us, &ui)) return;

    const uint32_t fps_milli = benchmark_fps_milli(ui);
    const uint32_t decode_avg_us = snapshot.frames_decoded != 0U
        ? static_cast<uint32_t>(snapshot.decode_us_total / snapshot.frames_decoded) : 0U;
    const uint32_t storage_avg_us = snapshot.frames_read != 0U
        ? static_cast<uint32_t>(snapshot.storage_us_total / snapshot.frames_read) : 0U;
    const uint32_t extract_avg_us = snapshot.frames_read != 0U
        ? static_cast<uint32_t>(snapshot.extract_us_total / snapshot.frames_read) : 0U;
    const uint32_t display_avg_us = ui.presented != 0U
        ? static_cast<uint32_t>(ui.display_us_total / ui.presented) : 0U;
    const uint32_t te_avg_us = ui.presented != 0U
        ? static_cast<uint32_t>(ui.te_us_total / ui.presented) : 0U;

    ESP_LOGI(TAG,
        "VideoBenchUI: present=%lu drop=%lu fps=%lu.%03lu src_decoded=%lu pool_skip=%lu jpeg_avg=%luus max=%luus TF_avg=%luus extract_avg=%luus display_avg=%luus max=%luus TE_avg=%luus PSRAM_free=%uKB",
        static_cast<unsigned long>(ui.presented),
        static_cast<unsigned long>(ui.dropped),
        static_cast<unsigned long>(fps_milli / 1000U),
        static_cast<unsigned long>(fps_milli % 1000U),
        static_cast<unsigned long>(snapshot.frames_decoded),
        static_cast<unsigned long>(snapshot.frames_pool_skipped),
        static_cast<unsigned long>(decode_avg_us),
        static_cast<unsigned long>(snapshot.decode_us_max),
        static_cast<unsigned long>(storage_avg_us),
        static_cast<unsigned long>(extract_avg_us),
        static_cast<unsigned long>(display_avg_us),
        static_cast<unsigned long>(ui.display_us_max),
        static_cast<unsigned long>(te_avg_us),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 1024U));

    if (snapshot.audio_streams != 0U) {
        const uint32_t audio_avg_bytes = snapshot.audio_frames_read != 0U
            ? static_cast<uint32_t>(snapshot.audio_compressed_bytes / snapshot.audio_frames_read) : 0U;
        const uint32_t audio_extract_avg_us = snapshot.audio_frames_read != 0U
            ? static_cast<uint32_t>(snapshot.audio_extract_us_total / snapshot.audio_frames_read) : 0U;
        const uint32_t audio_storage_avg_us = snapshot.audio_frames_read != 0U
            ? static_cast<uint32_t>(snapshot.audio_storage_us_total / snapshot.audio_frames_read) : 0U;
        const int32_t av0_delta_ms = (snapshot.audio_frames_read != 0U && snapshot.frames_decoded != 0U)
            ? static_cast<int32_t>(snapshot.audio_first_pts_ms) - static_cast<int32_t>(snapshot.first_pts_ms)
            : 0;
        ESP_LOGI(TAG,
            "AVIAudio: MP3 %luHz/%uch bits=%u bitrate=%lu frames=%lu avg=%luB max=%luB pts=%lu..%lu av0_delta=%ldms extract_avg=%luus TF_avg=%luus output=AudioTask",
            static_cast<unsigned long>(snapshot.audio_sample_rate),
            static_cast<unsigned>(snapshot.audio_channels),
            static_cast<unsigned>(snapshot.audio_bits_per_sample),
            static_cast<unsigned long>(snapshot.audio_bitrate),
            static_cast<unsigned long>(snapshot.audio_frames_read),
            static_cast<unsigned long>(audio_avg_bytes),
            static_cast<unsigned long>(snapshot.audio_compressed_bytes_max),
            static_cast<unsigned long>(snapshot.audio_first_pts_ms),
            static_cast<unsigned long>(snapshot.audio_last_pts_ms),
            static_cast<long>(av0_delta_ms),
            static_cast<unsigned long>(audio_extract_avg_us),
            static_cast<unsigned long>(audio_storage_avg_us));
    }

    if (ui.audio_master_active) {
        AudioVideoClockSnapshot audio_clock = {};
        if (audio_service_video_mp3_get_clock(&audio_clock) && audio_clock.active) {
            ESP_LOGI(TAG,
                "AVMaster: source=AudioPCM rev=%lu pcm=%llums submitted=%lluf audio_pts0=%lums probe_late_ref=AudioPCM eof=%u",
                static_cast<unsigned long>(audio_clock.revision),
                static_cast<unsigned long long>(audio_clock.position_us / 1000ULL),
                static_cast<unsigned long long>(audio_clock.submitted_frames),
                static_cast<unsigned long>(ui.audio_first_pts_ms),
                static_cast<unsigned>(audio_clock.eof));
        }
    }

    const CadenceProbeWindow &probe = ui.probe;
    const uint32_t wake_gap_avg_us = probe.tick_gap_samples != 0U
        ? static_cast<uint32_t>(probe.tick_gap_us_total / probe.tick_gap_samples) : 0U;
    const int32_t ready_late_avg_us = probe.ready_samples != 0U
        ? benchmark_probe_clamp_i32(probe.ready_late_us_total / probe.ready_samples) : 0;
    const uint32_t held_wait_avg_us = probe.held_samples != 0U
        ? static_cast<uint32_t>(probe.held_wait_us_total / probe.held_samples) : 0U;
    const int32_t start_late_avg_us = probe.present_start_samples != 0U
        ? benchmark_probe_clamp_i32(probe.present_start_late_us_total / probe.present_start_samples) : 0;
    const int32_t done_late_avg_us = probe.present_done_samples != 0U
        ? benchmark_probe_clamp_i32(probe.present_done_late_us_total / probe.present_done_samples) : 0;
    const uint32_t present_gap_avg_us = probe.present_gap_samples != 0U
        ? static_cast<uint32_t>(probe.present_gap_us_total / probe.present_gap_samples) : 0U;
    const uint32_t post_idle_avg_us = probe.post_idle_samples != 0U
        ? static_cast<uint32_t>(probe.post_idle_us_total / probe.post_idle_samples) : 0U;

    ESP_LOGI(TAG,
        "PresenterProbeA: wakes=%lu wake_gap=%lu/%luus wait_timeout=%lu pts_wait=%lu ready_late=%ldus[%ld..%ld] held_wait=%lu/%luus",
        static_cast<unsigned long>(probe.ticks),
        static_cast<unsigned long>(wake_gap_avg_us),
        static_cast<unsigned long>(probe.tick_gap_us_max),
        static_cast<unsigned long>(probe.take_miss),
        static_cast<unsigned long>(probe.arm_wait),
        static_cast<long>(ready_late_avg_us),
        static_cast<long>(probe.ready_late_us_min),
        static_cast<long>(probe.ready_late_us_max),
        static_cast<unsigned long>(held_wait_avg_us),
        static_cast<unsigned long>(probe.held_wait_us_max));
    ESP_LOGI(TAG,
        "PresenterProbeB: start_late=%ldus[%ld..%ld] done_late=%ldus[%ld..%ld] present_gap=%lu/%luus post_idle=%lu/%luus samples=%lu",
        static_cast<long>(start_late_avg_us),
        static_cast<long>(probe.present_start_late_us_min),
        static_cast<long>(probe.present_start_late_us_max),
        static_cast<long>(done_late_avg_us),
        static_cast<long>(probe.present_done_late_us_min),
        static_cast<long>(probe.present_done_late_us_max),
        static_cast<unsigned long>(present_gap_avg_us),
        static_cast<unsigned long>(probe.present_gap_us_max),
        static_cast<unsigned long>(post_idle_avg_us),
        static_cast<unsigned long>(probe.post_idle_us_max),
        static_cast<unsigned long>(probe.present_done_samples));
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
    if (benchmark_stop_avi_audio(terminal_reason)) {
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

    char line1[180] = {};
    char line2[180] = {};
    char line3[200] = {};
    if (pass) {
        snprintf(line1, sizeof(line1), "Native MJPEG Benchmark PASS\n显示 %lu 帧  丢帧 %lu",
            static_cast<unsigned long>(ui.presented),
            static_cast<unsigned long>(ui.dropped));
    } else {
        snprintf(line1, sizeof(line1), "Benchmark结束：%s\n错误：%s",
            VideoBenchmark::state_name(snapshot.state), esp_err_to_name(terminal_error));
    }
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

    lv_label_set_text(g_probe_video, line1);
    lv_label_set_text(g_probe_audio, line2);
    lv_label_set_text(g_probe_misc, line3);
    lv_obj_invalidate(g_root);

    ESP_LOGI(TAG,
        "MJPEG Benchmark UI结束：state=%s result=%s present=%lu drop=%lu fps=%lu.%03lu display_avg=%luus max=%luus stream_max=%luus TEmax=%luus display_fail=%lu",
        VideoBenchmark::state_name(snapshot.state), esp_err_to_name(terminal_error),
        static_cast<unsigned long>(ui.presented),
        static_cast<unsigned long>(ui.dropped),
        static_cast<unsigned long>(fps_milli / 1000U),
        static_cast<unsigned long>(fps_milli % 1000U),
        static_cast<unsigned long>(display_avg_us),
        static_cast<unsigned long>(ui.display_us_max),
        static_cast<unsigned long>(ui.stream_us_max),
        static_cast<unsigned long>(ui.te_us_max),
        static_cast<unsigned long>(ui.display_failures));
}

static void benchmark_tick()
{
    benchmark_cleanup_tick();
    if (g_page != VideoPage::Benchmark || g_benchmark_ui.terminal_shown) return;

    VideoBenchmark::Snapshot snapshot = {};
    if (!VideoBenchmark::get_snapshot(&snapshot)) return;
    benchmark_log_realtime(snapshot, esp_timer_get_time());

    if (!benchmark_presenter_is_running() &&
        (snapshot.state == VideoBenchmark::State::Eof ||
         snapshot.state == VideoBenchmark::State::Stopped ||
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
        if (!benchmark_request_stop("back")) return;
        show_browser();
        lv_obj_invalidate(g_root);
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

static void row_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click() || g_page != VideoPage::Browser ||
        g_browser_load.phase != BrowserLoadPhase::Idle) return;
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
    if (!benchmark_pause_music_for_exclusive()) {
        show_status("无法暂停当前Music，Video Exclusive未启动", 0xE18A8A);
        gesture_router_reset();
        return;
    }
    g_page = VideoPage::Benchmark;
    if (g_timer != nullptr) lv_timer_set_period(g_timer, kBenchmarkTimerPeriodMs);
    set_visible(g_browser_host, false);
    set_visible(g_probe_host, true);
    update_header();
    lv_label_set_text(g_probe_title, name);
    lv_label_set_text(g_probe_video, "准备 460x460 MJPEG 解码…");
    lv_label_set_text(g_probe_audio, "Dedicated Presenter + Unified PTS + CO5300 BoundedSPI");
    lv_label_set_text(g_probe_misc, "R.40.4.1：AVI MJPEG + MP3 Audio Pipeline V1；启动AVI前暂停Music\nMP3->32KB Bridge->AudioTask->PCM/CS43131，右滑停止后恢复Music");
    // 物理 GRAM 即将由 BoundedSPI 视频帧接管。提前隐藏 Video LVGL root，留出至少一个
    // decoder warm-up 窗口让 LVGL 消化隐藏 invalidation；播放期间不再产生覆盖视频的 repaint。
    set_visible(g_root, false);
    g_benchmark_ui = {};
    const esp_err_t ret = VideoBenchmark::start(g_selected_path);
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
    } else {
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
        }
    }
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
    lv_obj_set_width(g_probe_misc, 388); lv_label_set_long_mode(g_probe_misc, LV_LABEL_LONG_WRAP); lv_obj_align(g_probe_misc, LV_ALIGN_TOP_LEFT, 0, 238);
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
    ESP_LOGI(TAG, "Video进入Foreground：/VIDEO AVI Browser已显示；目录页Music保持后台，启动AVI时暂停Music并由AudioTask接管AVI MP3");
    return ESP_OK;
}

static esp_err_t video_leave(AppRunState next_state)
{
    (void)next_state;
    if (g_page == VideoPage::Benchmark && !benchmark_request_stop("leave")) return ESP_ERR_TIMEOUT;
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
    if (g_page == VideoPage::Benchmark || g_benchmark_ui.cleanup_pending) {
        benchmark_request_stop("destroy");
        const TickType_t wait_tick = pdMS_TO_TICKS(10);
        const uint32_t attempts = kDestroyCleanupWaitMs / 10U;
        for (uint32_t i = 0U; i < attempts; ++i) {
            if (VideoBenchmark::cleanup() == ESP_OK) {
                g_benchmark_ui.cleanup_pending = false;
                break;
            }
            vTaskDelay(wait_tick);
        }
        if (g_benchmark_ui.cleanup_pending) {
            ESP_LOGW(TAG, "Video destroy：Benchmark task仍在退出，PSRAM由下次Video create/start继续回收");
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
    if (g_root != nullptr) { lv_obj_delete(g_root); g_root = nullptr; }
    if (g_current_dir != nullptr) heap_caps_free(g_current_dir);
    if (g_scratch_path != nullptr) heap_caps_free(g_scratch_path);
    if (g_selected_path != nullptr) heap_caps_free(g_selected_path);
    g_current_dir = nullptr;
    g_scratch_path = nullptr;
    g_selected_path = nullptr;
    g_page = VideoPage::Browser;
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
        ESP_LOGI(TAG, "Video APP已注册：AVI Browser + Native MJPEG Dedicated Presenter + Audio Master A/V Sync V1");
    }
    return ret;
}
