#include "system_loop.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "boot_state.h"
#include "system_runtime.h"
#include "persistent_state.h"
#include "power_service.h"
#include "gpio0_service.h"
#include "screen_lock_simple.h"
#include "player_control.h"
#include "player_state.h"
#include "media_catalog_v2.h"
#include "artwork_loader.h"
#include "cover_surface_cache.h"
#include "app_diag_config.h"
#include "flac_decoder.h"
#include "audio_service.h"
#if APP_DIAG_MP3_PERFORMANCE
#include "mp3_decoder.h"
#endif

static const char *TAG =
    "系统";

// AudioTask 进入 Error 后每15秒重报最近一次故障快照。
// 平时完全无开销；这样偶发停播时即使当时没开串口，随后连接监视器仍能看到现场。
static constexpr TickType_t AUDIO_FAULT_REMINDER_INTERVAL = pdMS_TO_TICKS(15000);
static TickType_t g_last_audio_fault_reminder_tick = 0;

#if APP_DIAG_SYSTEM_HEARTBEAT || APP_DIAG_FLAC_PERFORMANCE || APP_DIAG_MP3_PERFORMANCE
static TickType_t g_last_diag_tick = 0;
#endif

#if APP_DIAG_FLAC_PERFORMANCE
static uint32_t g_last_flac_perf_sequence =
    0;
#endif

#if APP_DIAG_MP3_PERFORMANCE
static uint32_t g_last_mp3_perf_sequence =
    0;
#endif

// 封面资源采用 Current-Only 编排；system_loop 只负责当前曲：
// 读取当前 JPEG/PNG 压缩原图 -> CoverTask 生成 normal+dimmed -> 压缩原图释放。
// 不再计算/读取/预热下一曲，避免 PSRAM 中长期保存无效 next 资源。
enum class ArtworkCurrentStage : uint8_t
{
    Idle = 0,
    WaitCompressed,
    RetryCompressed,
    WaitSurface,
    Complete,
};

static uint32_t g_artwork_context_generation = 0U;
static uint32_t g_artwork_current_track = UINT32_MAX;
static ArtworkCurrentStage g_artwork_stage = ArtworkCurrentStage::Idle;

// 当前曲封面只在 SD 瞬态繁忙时退避；不再存在 next 的 750ms 慢重试。
static constexpr TickType_t ARTWORK_CURRENT_RETRY_BASE = pdMS_TO_TICKS(200);
static constexpr TickType_t ARTWORK_RETRY_MAX_DELAY = pdMS_TO_TICKS(5000);
static TickType_t g_artwork_retry_due_tick = 0;
static uint8_t g_artwork_current_retry_count = 0U;

// 音频不断流优先：FLAC ring >=90% 后 ArtworkTask 才继续抢短 SD 窗口。
// 不预热 next；所有可用窗口都只服务当前曲，避免后台预热竞争。
static constexpr uint32_t ARTWORK_CURRENT_FLAC_RING_MIN_PERCENT = 90U;
static constexpr TickType_t ARTWORK_STORAGE_WINDOW_LOG_INTERVAL = pdMS_TO_TICKS(1000);
static TickType_t g_artwork_storage_wait_last_log_tick = 0;

static bool system_artwork_storage_window_open(uint32_t track_index)
{
    FlacStorageWindowSnapshot window = {};
    if (!flac_decoder_get_storage_window(&window) || !window.active ||
        !window.storage_competes || window.capacity_bytes == 0U) {
        return true;
    }

    const uint64_t lhs = static_cast<uint64_t>(window.buffered_bytes) * 100ULL;
    const uint64_t rhs = static_cast<uint64_t>(window.capacity_bytes) *
        ARTWORK_CURRENT_FLAC_RING_MIN_PERCENT;
    if (lhs >= rhs) return true;

    const TickType_t now = xTaskGetTickCount();
    if (g_artwork_storage_wait_last_log_tick == 0 ||
        now - g_artwork_storage_wait_last_log_tick >= ARTWORK_STORAGE_WINDOW_LOG_INTERVAL) {
        g_artwork_storage_wait_last_log_tick = now;
        if (APP_DIAG_ARTWORK_UI) ESP_LOGI(TAG,
            "当前曲封面等待 FLAC 安全 I/O 窗口：track=%lu rate=%luHz ring=%lu/%luB threshold=%lu%%",
            static_cast<unsigned long>(track_index),
            static_cast<unsigned long>(window.sample_rate_hz),
            static_cast<unsigned long>(window.buffered_bytes),
            static_cast<unsigned long>(window.capacity_bytes),
            static_cast<unsigned long>(ARTWORK_CURRENT_FLAC_RING_MIN_PERCENT));
    }
    return false;
}

static bool system_tick_due(TickType_t now, TickType_t due)
{
    return static_cast<int32_t>(now - due) >= 0;
}

static TickType_t system_artwork_retry_delay(uint8_t retry_count)
{
    TickType_t delay = ARTWORK_CURRENT_RETRY_BASE;
    uint8_t shifts = retry_count > 0U ? static_cast<uint8_t>(retry_count - 1U) : 0U;
    if (shifts > 4U) shifts = 4U;
    while (shifts-- > 0U && delay < ARTWORK_RETRY_MAX_DELAY / 2U) delay *= 2U;
    return delay > ARTWORK_RETRY_MAX_DELAY ? ARTWORK_RETRY_MAX_DELAY : delay;
}

static void system_artwork_schedule_retry(uint32_t track_index)
{
    if (g_artwork_current_retry_count < UINT8_MAX) ++g_artwork_current_retry_count;
    const TickType_t delay = system_artwork_retry_delay(g_artwork_current_retry_count);
    g_artwork_retry_due_tick = xTaskGetTickCount() + delay;
    g_artwork_stage = ArtworkCurrentStage::RetryCompressed;
    ESP_LOGW(TAG, "当前曲封面暂未完成：track=%lu，第%u次退避%lums后重试",
        static_cast<unsigned long>(track_index),
        static_cast<unsigned>(g_artwork_current_retry_count),
        static_cast<unsigned long>(delay * portTICK_PERIOD_MS));
}

static bool system_cover_surface_cached(uint32_t track_index)
{
    CoverSurfaceLease lease = {};
    if (!cover_surface_cache_acquire(track_index, &lease)) return false;
    cover_surface_cache_release(&lease);
    return true;
}

static bool system_artwork_compressed_cached(uint32_t track_index)
{
    ArtworkCacheLease lease = {};
    if (!artwork_loader_acquire_cached(track_index, &lease)) return false;
    artwork_loader_release_cached(&lease);
    return true;
}

static void system_artwork_begin_context(uint32_t generation, uint32_t current_track)
{
    g_artwork_context_generation = generation;
    g_artwork_current_track = current_track;
    g_artwork_stage = ArtworkCurrentStage::Idle;
    g_artwork_retry_due_tick = 0;
    g_artwork_current_retry_count = 0U;
    g_artwork_storage_wait_last_log_tick = 0;

    if (cover_surface_cache_is_ready() && system_cover_surface_cached(current_track)) {
        g_artwork_stage = ArtworkCurrentStage::Complete;
    } else if (system_artwork_compressed_cached(current_track)) {
        if (cover_surface_cache_request_track(current_track, nullptr)) {
            g_artwork_stage = ArtworkCurrentStage::WaitSurface;
        } else {
            system_artwork_schedule_retry(current_track);
        }
    } else {
        ArtworkLoaderSnapshot loader = {};
        const bool same_inflight = artwork_loader_get_snapshot(&loader) &&
            loader.catalog_generation == generation && loader.track_index == current_track;
        if (same_inflight && loader.state == ArtworkLoadState::Loading) {
            g_artwork_stage = ArtworkCurrentStage::WaitCompressed;
        } else if (same_inflight && loader.state == ArtworkLoadState::Ready) {
            if (cover_surface_cache_request_track(current_track, nullptr)) {
                g_artwork_stage = ArtworkCurrentStage::WaitSurface;
            } else {
                system_artwork_schedule_retry(current_track);
            }
        } else if (same_inflight &&
                   (loader.state == ArtworkLoadState::NoArtwork || loader.state == ArtworkLoadState::Failed)) {
            g_artwork_stage = ArtworkCurrentStage::Complete;
        } else if (!system_artwork_storage_window_open(current_track)) {
            g_artwork_stage = ArtworkCurrentStage::RetryCompressed;
            g_artwork_retry_due_tick = 0;
        } else if (artwork_loader_request_track(current_track, nullptr)) {
            g_artwork_stage = ArtworkCurrentStage::WaitCompressed;
        } else {
            system_artwork_schedule_retry(current_track);
        }
    }

    if (APP_DIAG_ARTWORK_UI) ESP_LOGI(TAG, "封面资源编排：current=%lu next=disabled generation=%lu stage=%u",
        static_cast<unsigned long>(current_track),
        static_cast<unsigned long>(generation),
        static_cast<unsigned>(g_artwork_stage));
}

static void system_artwork_current_update()
{
    if (!artwork_loader_is_ready() || !cover_surface_cache_is_ready() ||
        !player_state_is_ready() || !media_catalog_v2_ready()) return;

    const uint32_t generation = media_catalog_v2_generation();
    const uint32_t current_track = static_cast<uint32_t>(player_state_get_index());
    if (generation != g_artwork_context_generation || current_track != g_artwork_current_track) {
        system_artwork_begin_context(generation, current_track);
    }

    switch (g_artwork_stage) {
        case ArtworkCurrentStage::WaitCompressed:
        {
            if (system_artwork_compressed_cached(current_track)) {
                if (cover_surface_cache_request_track(current_track, nullptr)) {
                    g_artwork_stage = ArtworkCurrentStage::WaitSurface;
                } else {
                    system_artwork_schedule_retry(current_track);
                }
                break;
            }
            ArtworkLoaderSnapshot snapshot = {};
            if (!artwork_loader_get_snapshot(&snapshot) ||
                snapshot.catalog_generation != generation || snapshot.track_index != current_track) break;
            if (snapshot.state == ArtworkLoadState::Failed && snapshot.result == ESP_ERR_TIMEOUT) {
                system_artwork_schedule_retry(current_track);
            } else if (snapshot.state == ArtworkLoadState::NoArtwork ||
                       snapshot.state == ArtworkLoadState::Failed) {
                g_artwork_stage = ArtworkCurrentStage::Complete;
            }
            break;
        }

        case ArtworkCurrentStage::RetryCompressed:
            if (!system_tick_due(xTaskGetTickCount(), g_artwork_retry_due_tick)) break;
            if (system_artwork_compressed_cached(current_track)) {
                if (cover_surface_cache_request_track(current_track, nullptr)) {
                    g_artwork_stage = ArtworkCurrentStage::WaitSurface;
                } else {
                    system_artwork_schedule_retry(current_track);
                }
            } else if (!system_artwork_storage_window_open(current_track)) {
                // 只等安全窗口，不累计 timeout/backoff。
            } else if (artwork_loader_request_track(current_track, nullptr)) {
                if (APP_DIAG_ARTWORK_UI) ESP_LOGI(TAG, "重试当前曲压缩封面：track=%lu",
                    static_cast<unsigned long>(current_track));
                g_artwork_stage = ArtworkCurrentStage::WaitCompressed;
            } else {
                system_artwork_schedule_retry(current_track);
            }
            break;

        case ArtworkCurrentStage::WaitSurface:
        {
            if (system_cover_surface_cached(current_track)) {
                g_artwork_stage = ArtworkCurrentStage::Complete;
                break;
            }
            CoverSurfaceSnapshot snapshot = {};
            if (!cover_surface_cache_get_snapshot(&snapshot) ||
                snapshot.catalog_generation != generation || snapshot.track_index != current_track) break;
            if (snapshot.state == CoverSurfaceState::Ready) {
                g_artwork_stage = ArtworkCurrentStage::Complete;
            } else if (snapshot.state == CoverSurfaceState::Failed) {
                if (snapshot.result == ESP_ERR_NO_MEM) {
                    // 两槽交换期间可能短暂同时被 pin，或 cache mutex 瞬态繁忙。Surface 插入失败
                    // 不能永久宣告 Complete，否则当前曲会一直停在压缩图 LVGL fallback，直到再次切歌。
                    // 保留压缩原图并走现有退避，槽位释放后自动重试 Surface。
                    system_artwork_schedule_retry(current_track);
                } else {
                    // 真正解码/格式失败保留压缩原图给 LVGL fallback；只有 Surface 成功才释放原图。
                    g_artwork_stage = ArtworkCurrentStage::Complete;
                }
            }
            break;
        }

        case ArtworkCurrentStage::Idle:
            system_artwork_begin_context(generation, current_track);
            break;

        case ArtworkCurrentStage::Complete:
        default:
            break;
    }
}


// ============================================================
// 系统主循环
// ============================================================

void system_loop_update()
{
    // 系统尚未启动完成时，不执行业务逻辑
    if (
        !boot_state_is_ready()
    ) {

        return;
    }

    // READY 之后第一轮业务循环统一启动可选后台服务。
    // 启动失败只进入降级运行，不反向破坏已经发布的系统 READY。
    system_runtime_update();


    // Player transport 只观察 AudioTask POD Snapshot；自然 EOF 的续播决策在 loopTask 执行，
    // AudioTask 本身不依赖 Player/Catalog，也不会直接选择下一首。
    player_control_update();

    // NVS V1 这里只同步 RAM 快照/dirty，不执行任何 Flash 写入。
    persistent_state_observe_runtime();

    // GPIO48 与 EC190707 共用电源键：放在 Player/持久化 RAM 快照更新之后，
    // 长按触发时可以 flush 本轮最新状态；不用 ISR，也不提前接管硬件最终断电。
    power_service_update();

    // GPIO0(K1) 辅助按键：音量- / 锁/解锁 / AOD / 熄屏（释放分级）。
    // 完全轮询实现，无 ISR；与 GPIO48 独立。
    gpio0_service_update();

    // 屏态渲染心跳：AOD 时钟每秒更新、防烧屏 30s ±1px 抖动。
    screen_lock_simple_render();

    AudioStateSnapshot audio_state = {};
    if (audio_service_get_snapshot(&audio_state) && audio_state.state == AudioPlaybackState::Error) {
        const TickType_t now = xTaskGetTickCount();
        if (g_last_audio_fault_reminder_tick == 0) {
            // 故障瞬间 AudioTask 已经打印过一次；从这里开始计时，15秒后才做低频重报。
            g_last_audio_fault_reminder_tick = now;
        } else if (now - g_last_audio_fault_reminder_tick >= AUDIO_FAULT_REMINDER_INTERVAL) {
            g_last_audio_fault_reminder_tick = now;
            audio_service_log_last_fault();
        }
    } else {
        g_last_audio_fault_reminder_tick = 0;
    }

    // 只编排当前曲封面；不读取/预热 next。压缩原图成功转成 Surface 后立即释放。
    system_artwork_current_update();

    // ========================================================
    // 以后这里负责调用：
    //
    // input_service_update();
    // player_service_update();
    // ui_service_update();
    // power_service_update();
    //
    // 但耗时任务不会放这里执行。
    // ========================================================


#if APP_DIAG_SYSTEM_HEARTBEAT || APP_DIAG_FLAC_PERFORMANCE || APP_DIAG_MP3_PERFORMANCE
    const TickType_t now = xTaskGetTickCount();
    if (now - g_last_diag_tick >= pdMS_TO_TICKS(5000)) {
        g_last_diag_tick = now;

#if APP_DIAG_SYSTEM_HEARTBEAT
        ESP_LOGI(
            TAG,
            "运行正常：内部 RAM=%u KB，PSRAM=%u KB",

            static_cast<unsigned>(
                heap_caps_get_free_size(
                    MALLOC_CAP_INTERNAL
                ) / 1024
            ),

            static_cast<unsigned>(
                heap_caps_get_free_size(
                    MALLOC_CAP_SPIRAM
                ) / 1024
            )
        );
#endif

#if APP_DIAG_FLAC_PERFORMANCE
        // FLAC 专项核查完成后默认不编译；需要回归高采样率性能时再打开编译期开关。
        FlacPerfSnapshot flac_perf = {};
        if (
            flac_decoder_get_perf_snapshot(&flac_perf) &&
            flac_perf.active &&
            flac_perf.sequence != g_last_flac_perf_sequence
        ) {

            g_last_flac_perf_sequence =
                flac_perf.sequence;


            ESP_LOGI(
                "FLAC",
                "PERF_TRACE: prefetch core=%ld fread avg=%luus max=%luus >10ms=%lu/%lu ring=%lu/%luB min=%luB copy avg=%luus max=%luus >2ms=%lu starve=%lu stack_hwm=%lu；decode avg=%luus max=%luus >20ms=%lu >40ms=%lu/%lu；refill avg=%luus max=%luus >20ms=%lu >30ms=%lu/%lu process/refill=%lu.%02lu max=%lu multi=%lu fill_max=%lu multi_fill=%lu window_compact=%lu/%luB topup_avg=%luB max=%luB；块预算=%luus over_budget=%lu worst_over=%luus margin=%ldus 负载avg=%lu%% peak=%lu%%",
                static_cast<long>(flac_perf.prefetch_core_id),
                static_cast<unsigned long>(flac_perf.read_avg_us),
                static_cast<unsigned long>(flac_perf.read_max_us),
                static_cast<unsigned long>(flac_perf.read_over_10ms),
                static_cast<unsigned long>(flac_perf.read_calls),
                static_cast<unsigned long>(flac_perf.ring_buffered_bytes),
                static_cast<unsigned long>(flac_perf.ring_capacity_bytes),
                static_cast<unsigned long>(flac_perf.ring_min_buffered_bytes),
                static_cast<unsigned long>(flac_perf.prefetch_copy_avg_us),
                static_cast<unsigned long>(flac_perf.prefetch_copy_max_us),
                static_cast<unsigned long>(flac_perf.prefetch_copy_over_2ms),
                static_cast<unsigned long>(flac_perf.prefetch_starve_count),
                static_cast<unsigned long>(flac_perf.prefetch_stack_hwm),
                static_cast<unsigned long>(flac_perf.decode_avg_us),
                static_cast<unsigned long>(flac_perf.decode_max_us),
                static_cast<unsigned long>(flac_perf.decode_over_20ms),
                static_cast<unsigned long>(flac_perf.decode_over_40ms),
                static_cast<unsigned long>(flac_perf.decode_calls),
                static_cast<unsigned long>(flac_perf.refill_avg_us),
                static_cast<unsigned long>(flac_perf.refill_max_us),
                static_cast<unsigned long>(flac_perf.refill_over_20ms),
                static_cast<unsigned long>(flac_perf.refill_over_30ms),
                static_cast<unsigned long>(flac_perf.refill_calls),
                static_cast<unsigned long>(flac_perf.process_per_refill_x100 / 100U),
                static_cast<unsigned long>(flac_perf.process_per_refill_x100 % 100U),
                static_cast<unsigned long>(flac_perf.process_per_refill_max),
                static_cast<unsigned long>(flac_perf.multi_process_refills),
                static_cast<unsigned long>(flac_perf.input_fills_per_refill_max),
                static_cast<unsigned long>(flac_perf.multi_fill_refills),
                static_cast<unsigned long>(flac_perf.input_compact_calls),
                static_cast<unsigned long>(flac_perf.input_compact_total_bytes),
                static_cast<unsigned long>(flac_perf.input_topup_avg_bytes),
                static_cast<unsigned long>(flac_perf.input_topup_max_bytes),
                static_cast<unsigned long>(flac_perf.block_budget_us),
                static_cast<unsigned long>(flac_perf.refill_over_block_budget),
                static_cast<unsigned long>(flac_perf.refill_worst_over_budget_us),
                static_cast<long>(flac_perf.refill_peak_margin_us),
                static_cast<unsigned long>(flac_perf.refill_avg_load_percent),
                static_cast<unsigned long>(flac_perf.refill_peak_load_percent)
            );
        }
#endif

#if APP_DIAG_MP3_PERFORMANCE
        // MP3 核查同样使用 Snapshot：AudioTask 只做轻量计时和 POD 发布，长日志在 loopTask 输出。
        Mp3PerfSnapshot mp3_perf = {};
        if (
            mp3_decoder_get_perf_snapshot(&mp3_perf) &&
            mp3_perf.active &&
            mp3_perf.sequence != g_last_mp3_perf_sequence
        ) {

            g_last_mp3_perf_sequence =
                mp3_perf.sequence;


            ESP_LOGI(
                "MP3",
                "PERF_TRACE: %luHz bitrate=%lu source_read avg=%luus max=%luus >5ms=%lu >10ms=%lu/%lu compact=%lu/%luB topup avg=%luB max=%luB；decode avg=%luus max=%luus >5ms=%lu >10ms=%lu/%lu；refill avg=%luus max=%luus >10ms=%lu >20ms=%lu/%lu process/refill=%lu.%02lu max=%lu multi=%lu fill_max=%lu multi_fill=%lu；PCM frames avg=%lu min=%lu max=%lu budget avg=%luus over_budget=%lu worst_over=%luus min_margin=%ldus 负载avg=%lu%% peak=%lu%%",
                static_cast<unsigned long>(mp3_perf.sample_rate_hz),
                static_cast<unsigned long>(mp3_perf.bitrate),
                static_cast<unsigned long>(mp3_perf.read_avg_us),
                static_cast<unsigned long>(mp3_perf.read_max_us),
                static_cast<unsigned long>(mp3_perf.read_over_5ms),
                static_cast<unsigned long>(mp3_perf.read_over_10ms),
                static_cast<unsigned long>(mp3_perf.read_calls),
                static_cast<unsigned long>(mp3_perf.compact_calls),
                static_cast<unsigned long>(mp3_perf.compact_total_bytes),
                static_cast<unsigned long>(mp3_perf.topup_avg_bytes),
                static_cast<unsigned long>(mp3_perf.topup_max_bytes),
                static_cast<unsigned long>(mp3_perf.decode_avg_us),
                static_cast<unsigned long>(mp3_perf.decode_max_us),
                static_cast<unsigned long>(mp3_perf.decode_over_5ms),
                static_cast<unsigned long>(mp3_perf.decode_over_10ms),
                static_cast<unsigned long>(mp3_perf.decode_calls),
                static_cast<unsigned long>(mp3_perf.refill_avg_us),
                static_cast<unsigned long>(mp3_perf.refill_max_us),
                static_cast<unsigned long>(mp3_perf.refill_over_10ms),
                static_cast<unsigned long>(mp3_perf.refill_over_20ms),
                static_cast<unsigned long>(mp3_perf.refill_calls),
                static_cast<unsigned long>(mp3_perf.process_per_refill_x100 / 100U),
                static_cast<unsigned long>(mp3_perf.process_per_refill_x100 % 100U),
                static_cast<unsigned long>(mp3_perf.process_per_refill_max),
                static_cast<unsigned long>(mp3_perf.multi_process_refills),
                static_cast<unsigned long>(mp3_perf.input_fills_per_refill_max),
                static_cast<unsigned long>(mp3_perf.multi_fill_refills),
                static_cast<unsigned long>(mp3_perf.output_frames_avg),
                static_cast<unsigned long>(mp3_perf.output_frames_min),
                static_cast<unsigned long>(mp3_perf.output_frames_max),
                static_cast<unsigned long>(mp3_perf.audio_budget_avg_us),
                static_cast<unsigned long>(mp3_perf.refill_over_budget),
                static_cast<unsigned long>(mp3_perf.refill_worst_over_budget_us),
                static_cast<long>(mp3_perf.refill_min_margin_us),
                static_cast<unsigned long>(mp3_perf.refill_avg_load_percent),
                static_cast<unsigned long>(mp3_perf.refill_peak_load_percent)
            );
        }
#endif
    }
#endif
}
