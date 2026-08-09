#include "system_loop.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "boot_state.h"
#include "player_control.h"
#include "player_state.h"
#include "media_catalog_v2.h"
#include "artwork_loader.h"
#include "cover_surface_cache.h"
#include "player_playlist.h"
#include "app_diag_config.h"
#include "flac_decoder.h"
#if APP_DIAG_MP3_PERFORMANCE
#include "mp3_decoder.h"
#endif

static const char *TAG =
    "系统";

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

// P1.2.6：资源预热由 system_loop 只做“编排”，不做 SD 读取、图片解码或缩放。
// 当前曲优先完成后，再依次预热下一曲，避免 latest-wins 队列覆盖当前曲请求。
enum class ArtworkPrewarmStage : uint8_t
{
    Idle = 0,
    WaitCurrentCompressed,
    RetryCurrentCompressed,
    WaitCurrentSurface,
    RequestNextCompressed,
    WaitNextCompressed,
    RetryNextCompressed,
    WaitNextSurface,
    Complete,
};

static uint32_t g_artwork_context_generation = 0U;
static uint32_t g_artwork_current_track = UINT32_MAX;
static uint32_t g_artwork_next_track = UINT32_MAX;
static ArtworkPrewarmStage g_artwork_prewarm_stage = ArtworkPrewarmStage::Idle;

// P1.2.7：SD 锁超时是“存储繁忙”的瞬态结果，不等于无封面。
// 当前曲短退避优先恢复；下一曲预热更保守，避免与高码率 FLAC 持续争 TF。
static constexpr TickType_t ARTWORK_CURRENT_RETRY_BASE = pdMS_TO_TICKS(200);
static constexpr TickType_t ARTWORK_NEXT_RETRY_BASE = pdMS_TO_TICKS(750);
static constexpr TickType_t ARTWORK_RETRY_MAX_DELAY = pdMS_TO_TICKS(5000);
static TickType_t g_artwork_retry_due_tick = 0;
static uint8_t g_artwork_current_retry_count = 0U;
static uint8_t g_artwork_next_retry_count = 0U;

// P1.2.8：高采样率 FLAC 不再靠 ArtworkTask 盲抢 SD mutex。
// 当前曲允许在 ring >=65% 时开始读取；下一曲预热更保守，要求 >=80%。
// 低于水位时只是等待，不记 timeout、不增加退避次数。
static constexpr uint32_t ARTWORK_CURRENT_FLAC_RING_MIN_PERCENT = 65U;
static constexpr uint32_t ARTWORK_NEXT_FLAC_RING_MIN_PERCENT = 80U;
static constexpr TickType_t ARTWORK_STORAGE_WINDOW_LOG_INTERVAL = pdMS_TO_TICKS(1000);
static TickType_t g_artwork_storage_wait_last_log_tick = 0;

static bool system_artwork_storage_window_open(bool current, uint32_t track_index)
{
    FlacStorageWindowSnapshot window = {};
    if (!flac_decoder_get_storage_window(&window) || !window.active ||
        !window.storage_competes || window.capacity_bytes == 0U) {
        return true;
    }

    const uint32_t min_percent = current
        ? ARTWORK_CURRENT_FLAC_RING_MIN_PERCENT
        : ARTWORK_NEXT_FLAC_RING_MIN_PERCENT;
    const uint64_t lhs = static_cast<uint64_t>(window.buffered_bytes) * 100ULL;
    const uint64_t rhs = static_cast<uint64_t>(window.capacity_bytes) * min_percent;
    if (lhs >= rhs) {
        return true;
    }

    const TickType_t now = xTaskGetTickCount();
    if (g_artwork_storage_wait_last_log_tick == 0 ||
        now - g_artwork_storage_wait_last_log_tick >= ARTWORK_STORAGE_WINDOW_LOG_INTERVAL) {
        g_artwork_storage_wait_last_log_tick = now;
        ESP_LOGI(TAG,
            "%s封面等待 FLAC 安全 I/O 窗口：track=%lu rate=%luHz ring=%lu/%luB threshold=%lu%%",
            current ? "当前曲" : "下一曲预热",
            static_cast<unsigned long>(track_index),
            static_cast<unsigned long>(window.sample_rate_hz),
            static_cast<unsigned long>(window.buffered_bytes),
            static_cast<unsigned long>(window.capacity_bytes),
            static_cast<unsigned long>(min_percent));
    }
    return false;
}

static bool system_tick_due(TickType_t now, TickType_t due)
{
    return static_cast<int32_t>(now - due) >= 0;
}

static TickType_t system_artwork_retry_delay(TickType_t base, uint8_t retry_count)
{
    TickType_t delay = base;
    uint8_t shifts = retry_count > 0U ? static_cast<uint8_t>(retry_count - 1U) : 0U;
    if (shifts > 4U) {
        shifts = 4U;
    }
    while (shifts-- > 0U && delay < ARTWORK_RETRY_MAX_DELAY / 2U) {
        delay *= 2U;
    }
    return delay > ARTWORK_RETRY_MAX_DELAY ? ARTWORK_RETRY_MAX_DELAY : delay;
}

static void system_artwork_schedule_retry(bool current, uint32_t track_index)
{
    uint8_t &count = current ? g_artwork_current_retry_count : g_artwork_next_retry_count;
    if (count < UINT8_MAX) {
        ++count;
    }
    const TickType_t delay = system_artwork_retry_delay(
        current ? ARTWORK_CURRENT_RETRY_BASE : ARTWORK_NEXT_RETRY_BASE, count);
    g_artwork_retry_due_tick = xTaskGetTickCount() + delay;
    g_artwork_prewarm_stage = current
        ? ArtworkPrewarmStage::RetryCurrentCompressed
        : ArtworkPrewarmStage::RetryNextCompressed;
    ESP_LOGW(TAG, "%s封面读取遇到存储繁忙：track=%lu，第%u次退避%lums后重试",
        current ? "当前曲" : "下一曲预热",
        static_cast<unsigned long>(track_index),
        static_cast<unsigned>(count),
        static_cast<unsigned long>(delay * portTICK_PERIOD_MS));
}

static bool system_cover_surface_cached(uint32_t track_index)
{
    CoverSurfaceLease lease = {};
    if (!cover_surface_cache_acquire(track_index, &lease)) {
        return false;
    }
    cover_surface_cache_release(&lease);
    return true;
}

static bool system_artwork_compressed_cached(uint32_t track_index)
{
    ArtworkCacheLease lease = {};
    if (!artwork_loader_acquire_cached(track_index, &lease)) {
        return false;
    }
    artwork_loader_release_cached(&lease);
    return true;
}

static void system_artwork_begin_context(uint32_t generation, uint32_t current_track)
{
    g_artwork_context_generation = generation;
    g_artwork_current_track = current_track;
    g_artwork_next_track = UINT32_MAX;
    g_artwork_prewarm_stage = ArtworkPrewarmStage::Idle;
    g_artwork_retry_due_tick = 0;
    g_artwork_current_retry_count = 0U;
    g_artwork_next_retry_count = 0U;
    g_artwork_storage_wait_last_log_tick = 0;

    PlayerListSnapshot list = {};
    if (player_playlist_get_snapshot(&list) && list.track_count > 1U &&
        list.catalog_generation == generation) {
        const size_t next_position = (static_cast<size_t>(list.position) + 1U) %
            static_cast<size_t>(list.track_count);
        size_t next_track = 0U;
        if (player_playlist_get_track_index_at_position(next_position, &next_track)) {
            const uint32_t next_track_u32 = static_cast<uint32_t>(next_track);
            if (static_cast<size_t>(next_track_u32) == next_track && next_track_u32 != current_track) {
                g_artwork_next_track = next_track_u32;
            }
        }
    }

    // 当前最终 Surface 若已命中，直接进入下一曲预热。若压缩封面已经被前一轮预取到，
    // 直接进入 Surface 阶段，避免快速切歌时对同一 Track 重启 SD 读取。
    if (cover_surface_cache_is_ready() && system_cover_surface_cached(current_track)) {
        g_artwork_prewarm_stage = ArtworkPrewarmStage::RequestNextCompressed;
    } else if (system_artwork_compressed_cached(current_track)) {
        if (cover_surface_cache_request_track(current_track, nullptr)) {
            g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitCurrentSurface;
        }
    } else {
        ArtworkLoaderSnapshot loader = {};
        const bool same_inflight = artwork_loader_get_snapshot(&loader) &&
            loader.catalog_generation == generation && loader.track_index == current_track;
        if (same_inflight && loader.state == ArtworkLoadState::Loading) {
            g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitCurrentCompressed;
        } else if (same_inflight && loader.state == ArtworkLoadState::Ready) {
            if (cover_surface_cache_request_track(current_track, nullptr)) {
                g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitCurrentSurface;
            }
        } else if (same_inflight &&
                   (loader.state == ArtworkLoadState::NoArtwork || loader.state == ArtworkLoadState::Failed)) {
            g_artwork_prewarm_stage = ArtworkPrewarmStage::RequestNextCompressed;
        } else if (!system_artwork_storage_window_open(true, current_track)) {
            g_artwork_prewarm_stage = ArtworkPrewarmStage::RetryCurrentCompressed;
            g_artwork_retry_due_tick = 0;
        } else if (artwork_loader_request_track(current_track, nullptr)) {
            g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitCurrentCompressed;
        }
    }

    if (g_artwork_next_track == UINT32_MAX) {
        ESP_LOGI(TAG, "封面资源编排：current=%lu next=none generation=%lu stage=%u",
            static_cast<unsigned long>(current_track),
            static_cast<unsigned long>(generation),
            static_cast<unsigned>(g_artwork_prewarm_stage));
    } else {
        ESP_LOGI(TAG, "封面资源编排：current=%lu next=%lu generation=%lu stage=%u",
            static_cast<unsigned long>(current_track),
            static_cast<unsigned long>(g_artwork_next_track),
            static_cast<unsigned long>(generation),
            static_cast<unsigned>(g_artwork_prewarm_stage));
    }
}

static void system_artwork_prewarm_update()
{
    if (!artwork_loader_is_ready() || !cover_surface_cache_is_ready() ||
        !player_state_is_ready() || !media_catalog_v2_ready()) {
        return;
    }

    const uint32_t generation = media_catalog_v2_generation();
    const uint32_t current_track = static_cast<uint32_t>(player_state_get_index());
    if (generation != g_artwork_context_generation || current_track != g_artwork_current_track) {
        system_artwork_begin_context(generation, current_track);
    }

    if (generation != g_artwork_context_generation || current_track != g_artwork_current_track) {
        return;
    }

    switch (g_artwork_prewarm_stage) {
        case ArtworkPrewarmStage::WaitCurrentCompressed:
        {
            if (system_artwork_compressed_cached(current_track)) {
                if (cover_surface_cache_request_track(current_track, nullptr)) {
                    g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitCurrentSurface;
                }
                break;
            }
            ArtworkLoaderSnapshot snapshot = {};
            if (!artwork_loader_get_snapshot(&snapshot) ||
                snapshot.catalog_generation != generation || snapshot.track_index != current_track) {
                break;
            }
            if (snapshot.state == ArtworkLoadState::Failed && snapshot.result == ESP_ERR_TIMEOUT) {
                system_artwork_schedule_retry(true, current_track);
            } else if (snapshot.state == ArtworkLoadState::NoArtwork ||
                       snapshot.state == ArtworkLoadState::Failed) {
                g_artwork_prewarm_stage = ArtworkPrewarmStage::RequestNextCompressed;
            }
            break;
        }

        case ArtworkPrewarmStage::RetryCurrentCompressed:
        {
            if (!system_tick_due(xTaskGetTickCount(), g_artwork_retry_due_tick)) {
                break;
            }
            if (system_artwork_compressed_cached(current_track)) {
                if (cover_surface_cache_request_track(current_track, nullptr)) {
                    g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitCurrentSurface;
                }
            } else if (!system_artwork_storage_window_open(true, current_track)) {
                break;
            } else if (artwork_loader_request_track(current_track, nullptr)) {
                ESP_LOGI(TAG, "重试当前曲封面：track=%lu",
                    static_cast<unsigned long>(current_track));
                g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitCurrentCompressed;
            }
            break;
        }

        case ArtworkPrewarmStage::WaitCurrentSurface:
        {
            CoverSurfaceSnapshot snapshot = {};
            if (!cover_surface_cache_get_snapshot(&snapshot) ||
                snapshot.catalog_generation != generation || snapshot.track_index != current_track) {
                break;
            }
            if (snapshot.state == CoverSurfaceState::Ready || snapshot.state == CoverSurfaceState::Failed) {
                g_artwork_prewarm_stage = ArtworkPrewarmStage::RequestNextCompressed;
            }
            break;
        }

        case ArtworkPrewarmStage::RequestNextCompressed:
        {
            if (g_artwork_next_track == UINT32_MAX || g_artwork_next_track == current_track) {
                g_artwork_prewarm_stage = ArtworkPrewarmStage::Complete;
                break;
            }
            if (system_cover_surface_cached(g_artwork_next_track)) {
                ESP_LOGI(TAG, "下一曲封面预热命中：track=%lu",
                    static_cast<unsigned long>(g_artwork_next_track));
                g_artwork_prewarm_stage = ArtworkPrewarmStage::Complete;
                break;
            }
            if (system_artwork_compressed_cached(g_artwork_next_track)) {
                if (cover_surface_cache_request_track(g_artwork_next_track, nullptr)) {
                    ESP_LOGI(TAG, "下一曲压缩封面已命中，开始生成最终 Surface：track=%lu",
                        static_cast<unsigned long>(g_artwork_next_track));
                    g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitNextSurface;
                }
                break;
            }

            ArtworkLoaderSnapshot loader = {};
            const bool same_inflight = artwork_loader_get_snapshot(&loader) &&
                loader.catalog_generation == generation && loader.track_index == g_artwork_next_track;
            if (same_inflight && loader.state == ArtworkLoadState::Loading) {
                g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitNextCompressed;
            } else if (same_inflight && loader.state == ArtworkLoadState::Ready) {
                if (cover_surface_cache_request_track(g_artwork_next_track, nullptr)) {
                    g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitNextSurface;
                }
            } else if (same_inflight && loader.state == ArtworkLoadState::Failed &&
                       loader.result == ESP_ERR_TIMEOUT) {
                system_artwork_schedule_retry(false, g_artwork_next_track);
            } else if (same_inflight &&
                       (loader.state == ArtworkLoadState::NoArtwork || loader.state == ArtworkLoadState::Failed)) {
                g_artwork_prewarm_stage = ArtworkPrewarmStage::Complete;
            } else if (!system_artwork_storage_window_open(false, g_artwork_next_track)) {
                g_artwork_prewarm_stage = ArtworkPrewarmStage::RetryNextCompressed;
                g_artwork_retry_due_tick = 0;
            } else if (artwork_loader_request_track(g_artwork_next_track, nullptr)) {
                ESP_LOGI(TAG, "开始预热下一曲压缩封面：track=%lu",
                    static_cast<unsigned long>(g_artwork_next_track));
                g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitNextCompressed;
            }
            break;
        }

        case ArtworkPrewarmStage::WaitNextCompressed:
        {
            if (system_artwork_compressed_cached(g_artwork_next_track)) {
                if (cover_surface_cache_request_track(g_artwork_next_track, nullptr)) {
                    g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitNextSurface;
                }
                break;
            }
            ArtworkLoaderSnapshot snapshot = {};
            if (!artwork_loader_get_snapshot(&snapshot) ||
                snapshot.catalog_generation != generation || snapshot.track_index != g_artwork_next_track) {
                break;
            }
            if (snapshot.state == ArtworkLoadState::Failed && snapshot.result == ESP_ERR_TIMEOUT) {
                system_artwork_schedule_retry(false, g_artwork_next_track);
            } else if (snapshot.state == ArtworkLoadState::NoArtwork ||
                       snapshot.state == ArtworkLoadState::Failed) {
                g_artwork_prewarm_stage = ArtworkPrewarmStage::Complete;
            }
            break;
        }

        case ArtworkPrewarmStage::RetryNextCompressed:
        {
            if (g_artwork_next_track == UINT32_MAX || g_artwork_next_track == current_track) {
                g_artwork_prewarm_stage = ArtworkPrewarmStage::Complete;
                break;
            }
            if (!system_tick_due(xTaskGetTickCount(), g_artwork_retry_due_tick)) {
                break;
            }
            if (system_cover_surface_cached(g_artwork_next_track)) {
                g_artwork_prewarm_stage = ArtworkPrewarmStage::Complete;
            } else if (system_artwork_compressed_cached(g_artwork_next_track)) {
                if (cover_surface_cache_request_track(g_artwork_next_track, nullptr)) {
                    g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitNextSurface;
                }
            } else if (!system_artwork_storage_window_open(false, g_artwork_next_track)) {
                break;
            } else if (artwork_loader_request_track(g_artwork_next_track, nullptr)) {
                ESP_LOGI(TAG, "重试下一曲封面预热：track=%lu",
                    static_cast<unsigned long>(g_artwork_next_track));
                g_artwork_prewarm_stage = ArtworkPrewarmStage::WaitNextCompressed;
            }
            break;
        }

        case ArtworkPrewarmStage::WaitNextSurface:
        {
            CoverSurfaceSnapshot snapshot = {};
            if (!cover_surface_cache_get_snapshot(&snapshot) ||
                snapshot.catalog_generation != generation || snapshot.track_index != g_artwork_next_track) {
                break;
            }
            if (snapshot.state == CoverSurfaceState::Ready) {
                ESP_LOGI(TAG, "下一曲最终封面预热完成：track=%lu prepare=%lums",
                    static_cast<unsigned long>(g_artwork_next_track),
                    static_cast<unsigned long>(snapshot.prepare_ms));
                g_artwork_prewarm_stage = ArtworkPrewarmStage::Complete;
            } else if (snapshot.state == CoverSurfaceState::Failed) {
                g_artwork_prewarm_stage = ArtworkPrewarmStage::Complete;
            }
            break;
        }

        case ArtworkPrewarmStage::Idle:
        case ArtworkPrewarmStage::Complete:
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


    // Player transport 只观察 AudioTask POD Snapshot；自然 EOF 的续播决策在 loopTask 执行，
    // AudioTask 本身不依赖 Player/Catalog，也不会直接选择下一首。
    player_control_update();

    // P1.2.6：资源编排只投递异步请求。当前曲完成后再预热下一曲，
    // 让实际切歌优先命中 460x460 RGB565 Surface；所有重活仍留在 Core1 后台任务。
    system_artwork_prewarm_update();


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
                "PERF_TRACE: %luHz bitrate=%lu fread avg=%luus max=%luus >5ms=%lu >10ms=%lu/%lu compact=%lu/%luB topup avg=%luB max=%luB；decode avg=%luus max=%luus >5ms=%lu >10ms=%lu/%lu；refill avg=%luus max=%luus >10ms=%lu >20ms=%lu/%lu process/refill=%lu.%02lu max=%lu multi=%lu fill_max=%lu multi_fill=%lu；PCM frames avg=%lu min=%lu max=%lu budget avg=%luus over_budget=%lu worst_over=%luus min_margin=%ldus 负载avg=%lu%% peak=%lu%%",
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
