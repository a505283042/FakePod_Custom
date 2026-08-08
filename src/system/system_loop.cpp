#include "system_loop.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "boot_state.h"
#include "audio_diag_config.h"
#if APP_DIAG_FLAC_PERFORMANCE
#include "flac_decoder.h"
#endif
#if APP_DIAG_MP3_PERFORMANCE
#include "mp3_decoder.h"
#endif

static const char *TAG =
    "系统";

static TickType_t g_last_alive_tick =
    0;

#if APP_DIAG_FLAC_PERFORMANCE
static uint32_t g_last_flac_perf_sequence =
    0;
#endif

#if APP_DIAG_MP3_PERFORMANCE
static uint32_t g_last_mp3_perf_sequence =
    0;
#endif


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


    TickType_t now =
        xTaskGetTickCount();


    // 每 5 秒打印一次系统状态
    if (
        now - g_last_alive_tick >=
        pdMS_TO_TICKS(5000)
    ) {

        g_last_alive_tick =
            now;


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
}
