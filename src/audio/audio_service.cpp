#include "audio_service.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cs43131.h"
#include "i2s_output.h"
#include "wav_decoder.h"

static const char *TAG = "音频服务";

static constexpr uint32_t AUDIO_TASK_STACK_BYTES = 8192;
static constexpr UBaseType_t AUDIO_TASK_PRIORITY = 5;
static constexpr BaseType_t AUDIO_TASK_CORE = 0;
static constexpr UBaseType_t AUDIO_COMMAND_QUEUE_LENGTH = 8;
static constexpr size_t AUDIO_INLINE_PATH_SIZE = 384;
static constexpr size_t AUDIO_MAX_PATH_SIZE = 4096;
static constexpr size_t AUDIO_STREAM_FRAMES = 256;
static constexpr uint32_t AUDIO_I2S_WRITE_TIMEOUT_MS = 100;
static constexpr uint32_t AUDIO_PCM_MUTE_SETTLE_MS = 150;
static constexpr TickType_t AUDIO_QUEUE_SEND_TIMEOUT = pdMS_TO_TICKS(50);
static constexpr TickType_t AUDIO_SYNC_WAIT_TIMEOUT = pdMS_TO_TICKS(1500);
static constexpr TickType_t AUDIO_START_WAIT_TIMEOUT = pdMS_TO_TICKS(1500);

// Stage 9.2 第一版输出保持 Stage 8.3 已实机验证过的安全低音量：
// CS43131 0.5Vrms 满量程 + PCM -40dB。后续再独立做用户音量系统。


enum class AudioCommandType : uint8_t
{
    Play = 1,
    Stop,
    Pause,
    Resume
};

struct AudioRequest
{
    AudioCommandType type = AudioCommandType::Stop;
    uint32_t request_id = 0;
    uint32_t track_index = UINT32_MAX;
    MediaFormat format = MediaFormat::Unknown;
    char path[AUDIO_INLINE_PATH_SIZE] = {};
    char *extended_path = nullptr;
    SemaphoreHandle_t done = nullptr;
    bool success = false;
    esp_err_t result = ESP_FAIL;
    uint8_t refs = 1;
};

static QueueHandle_t g_command_queue = nullptr;
static TaskHandle_t g_audio_task = nullptr;
static SemaphoreHandle_t g_start_done = nullptr;
static esp_err_t g_start_result = ESP_ERR_INVALID_STATE;

static portMUX_TYPE g_request_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_next_request_id = 1;

static portMUX_TYPE g_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static AudioStateSnapshot g_snapshot = {};

// 以下状态只允许 AudioTask 自己写。
static bool g_task_ready = false;
static AudioPlaybackState g_task_state = AudioPlaybackState::Starting;
static uint32_t g_task_state_revision = 0;
static uint32_t g_task_playback_revision = 1;
static uint32_t g_task_last_request_id = 0;
static uint32_t g_task_track_index = UINT32_MAX;
static MediaFormat g_task_format = MediaFormat::Unknown;
static esp_err_t g_task_last_error = ESP_OK;
static uint32_t g_task_sample_rate_hz = 0;
static uint16_t g_task_channels = 0;
static uint16_t g_task_bits_per_sample = 0;
static uint64_t g_task_position_frames = 0;
static uint64_t g_task_total_frames = 0;
static uint64_t g_last_progress_publish_frame = 0;

// 正式播放资源也只属于 AudioTask。
static WavDecoder g_wav = {};
static bool g_pipeline_clock_prepared = false;
static bool g_pipeline_i2s_started = false;
static bool g_pipeline_asp_enabled = false;
static bool g_pipeline_headphone_enabled = false;
static int32_t g_pcm_block[AUDIO_STREAM_FRAMES * 2] = {};

static const char *audio_request_path(const AudioRequest *request)
{
    if (request == nullptr) {
        return nullptr;
    }
    return request->extended_path != nullptr ? request->extended_path : request->path;
}

static void audio_request_retain(AudioRequest *request)
{
    if (request == nullptr) {
        return;
    }
    portENTER_CRITICAL(&g_request_mux);
    request->refs++;
    portEXIT_CRITICAL(&g_request_mux);
}

static void audio_request_release(AudioRequest *request)
{
    if (request == nullptr) {
        return;
    }

    bool destroy = false;
    portENTER_CRITICAL(&g_request_mux);
    if (request->refs > 0) {
        request->refs--;
        destroy = request->refs == 0;
    }
    portEXIT_CRITICAL(&g_request_mux);

    if (!destroy) {
        return;
    }

    if (request->done != nullptr) {
        vSemaphoreDelete(request->done);
        request->done = nullptr;
    }
    if (request->extended_path != nullptr) {
        heap_caps_free(request->extended_path);
        request->extended_path = nullptr;
    }
    heap_caps_free(request);
}

static AudioRequest *audio_request_create(AudioCommandType type, bool wait)
{
    AudioRequest *request = static_cast<AudioRequest *>(
        heap_caps_calloc(1, sizeof(AudioRequest), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (request == nullptr) {
        request = static_cast<AudioRequest *>(calloc(1, sizeof(AudioRequest)));
    }
    if (request == nullptr) {
        return nullptr;
    }

    request->type = type;
    request->refs = 1;
    request->track_index = UINT32_MAX;
    request->format = MediaFormat::Unknown;
    request->result = ESP_FAIL;

    portENTER_CRITICAL(&g_request_mux);
    request->request_id = g_next_request_id++;
    if (g_next_request_id == 0) {
        g_next_request_id = 1;
    }
    portEXIT_CRITICAL(&g_request_mux);

    if (wait) {
        request->done = xSemaphoreCreateBinary();
        if (request->done == nullptr) {
            audio_request_release(request);
            return nullptr;
        }
    }
    return request;
}

static bool audio_request_set_path(AudioRequest *request, const char *path)
{
    if (request == nullptr || path == nullptr) {
        return false;
    }

    const size_t length = strlen(path);
    if (length > AUDIO_MAX_PATH_SIZE) {
        ESP_LOGE(TAG, "路径过长，拒绝入队：长度=%u，上限=%u，请求=%lu",
            static_cast<unsigned>(length),
            static_cast<unsigned>(AUDIO_MAX_PATH_SIZE),
            static_cast<unsigned long>(request->request_id));
        return false;
    }

    if (length < sizeof(request->path)) {
        memcpy(request->path, path, length + 1);
        return true;
    }

    request->extended_path = static_cast<char *>(
        heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (request->extended_path == nullptr) {
        request->extended_path = static_cast<char *>(malloc(length + 1));
    }
    if (request->extended_path == nullptr) {
        ESP_LOGE(TAG, "长路径分配失败：长度=%u，请求=%lu",
            static_cast<unsigned>(length),
            static_cast<unsigned long>(request->request_id));
        return false;
    }

    memcpy(request->extended_path, path, length + 1);
    ESP_LOGI(TAG, "长路径使用动态缓冲：长度=%u，请求=%lu",
        static_cast<unsigned>(length),
        static_cast<unsigned long>(request->request_id));
    return true;
}

static void audio_task_advance_playback_revision()
{
    g_task_playback_revision++;
    if (g_task_playback_revision == 0) {
        g_task_playback_revision = 1;
    }
}

static void audio_task_publish_snapshot()
{
    AudioStateSnapshot snapshot = {};
    snapshot.ready = g_task_ready;
    snapshot.state = g_task_state;
    snapshot.state_revision = ++g_task_state_revision;
    if (snapshot.state_revision == 0) {
        snapshot.state_revision = ++g_task_state_revision;
    }
    snapshot.playback_revision = g_task_playback_revision;
    snapshot.last_request_id = g_task_last_request_id;
    snapshot.track_index = g_task_track_index;
    snapshot.format = g_task_format;
    snapshot.last_error = g_task_last_error;
    snapshot.queue_depth = g_command_queue != nullptr
        ? static_cast<uint32_t>(uxQueueMessagesWaiting(g_command_queue))
        : 0;
    snapshot.sample_rate_hz = g_task_sample_rate_hz;
    snapshot.channels = g_task_channels;
    snapshot.bits_per_sample = g_task_bits_per_sample;
    snapshot.position_frames = g_task_position_frames;
    snapshot.total_frames = g_task_total_frames;

    portENTER_CRITICAL(&g_snapshot_mux);
    g_snapshot = snapshot;
    portEXIT_CRITICAL(&g_snapshot_mux);
}

static void audio_task_set_state(AudioPlaybackState state, esp_err_t error = ESP_OK)
{
    g_task_state = state;
    g_task_last_error = error;
    audio_task_publish_snapshot();
}

static void audio_task_reset_media_fields()
{
    g_task_sample_rate_hz = 0;
    g_task_channels = 0;
    g_task_bits_per_sample = 0;
    g_task_position_frames = 0;
    g_task_total_frames = 0;
    g_last_progress_publish_frame = 0;
}

static void audio_request_complete(AudioRequest *request, bool success, esp_err_t result)
{
    if (request == nullptr) {
        return;
    }
    request->success = success;
    request->result = result;
    if (request->done != nullptr) {
        xSemaphoreGive(request->done);
    }
}

static void audio_task_remember_first_error(esp_err_t candidate, esp_err_t *first_error)
{
    if (first_error != nullptr && *first_error == ESP_OK && candidate != ESP_OK) {
        *first_error = candidate;
    }
}

static esp_err_t audio_task_shutdown_pipeline()
{
    esp_err_t first_error = ESP_OK;

    // 先触发 PCM 软斜坡静音，并在整个静音收敛窗口持续发送全零 PCM。
    // CS43131 的 PCM_SZC=soft-ramp 模式不是“写寄存器后立即静音”；官方多个切换序列
    // 都为软斜坡静音预留 150ms。必须等斜坡完成后再置 PDN_HP，否则长时间真实播放后
    // 可能出现 PDN_DONE 未在短窗口内到达，而短测试音却偶尔能立即完成的现象。
    if (g_pipeline_headphone_enabled) {
        esp_err_t mute_ret = cs43131_set_pcm_mute(true);
        audio_task_remember_first_error(mute_ret, &first_error);

        if (mute_ret == ESP_OK) {
            const uint32_t settle_rate = g_task_sample_rate_hz > 0 ? g_task_sample_rate_hz : 48000U;
            const size_t settle_frames = static_cast<size_t>(
                (static_cast<uint64_t>(settle_rate) * AUDIO_PCM_MUTE_SETTLE_MS + 999ULL) / 1000ULL
            );

            if (g_pipeline_i2s_started && i2s_output_is_started()) {
                ESP_LOGI(TAG, "PCM软静音收敛：保持%lums全零PCM，帧=%u",
                    static_cast<unsigned long>(AUDIO_PCM_MUTE_SETTLE_MS),
                    static_cast<unsigned>(settle_frames));
                audio_task_remember_first_error(
                    i2s_output_stream_write_silence(settle_frames, AUDIO_I2S_WRITE_TIMEOUT_MS),
                    &first_error);
            } else {
                // 异常清理路径没有 I2S 时仍给模拟静音电路留出收敛时间。
                vTaskDelay(pdMS_TO_TICKS(AUDIO_PCM_MUTE_SETTLE_MS));
            }
        }

        audio_task_remember_first_error(cs43131_power_down_headphone_playback(), &first_error);
        g_pipeline_headphone_enabled = false;
        g_pipeline_asp_enabled = false;
    }

    // 即使 ASP/耳放启动过程中失败，也用统一 finish 将 ASP、XTAL 恢复到安全待机状态。
    if (g_pipeline_clock_prepared || g_pipeline_asp_enabled) {
        audio_task_remember_first_error(cs43131_finish_pcm_playback(), &first_error);
        g_pipeline_clock_prepared = false;
        g_pipeline_asp_enabled = false;
    }

    if (g_pipeline_i2s_started || i2s_output_is_started()) {
        audio_task_remember_first_error(i2s_output_stop(), &first_error);
        g_pipeline_i2s_started = false;
    }

    if (wav_decoder_is_open(&g_wav)) {
        wav_decoder_close(&g_wav);
    }

    return first_error;
}

static esp_err_t audio_task_start_wav_pipeline(const char *path)
{
    esp_err_t ret = wav_decoder_open(&g_wav, path);
    if (ret != ESP_OK) {
        return ret;
    }

    g_task_sample_rate_hz = g_wav.sample_rate_hz;
    g_task_channels = g_wav.channels;
    g_task_bits_per_sample = g_wav.bits_per_sample;
    g_task_position_frames = 0;
    g_task_total_frames = g_wav.total_frames;
    g_last_progress_publish_frame = 0;
    audio_task_publish_snapshot();

    // 从这一刻开始即使 CS43131 准备过程中途失败，也必须执行 finish 清理。
    g_pipeline_clock_prepared = true;
    ret = cs43131_prepare_pcm_playback_32bit(g_wav.sample_rate_hz);
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }

    ret = i2s_output_stream_start_32bit(g_wav.sample_rate_hz);
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }
    g_pipeline_i2s_started = true;

    // 先给 I2S DMA 填入全零 PCM，再开启 CS43131 ASP，避免 ASP 上电瞬间面对不稳定时钟。
    ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }

    ret = cs43131_enable_asp_input();
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }
    g_pipeline_asp_enabled = true;

    // 保持约 15ms 全零 PCM，让 ASP 锁定 BCLK/LRCK。
    for (int i = 0; i < 3; ++i) {
        ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            audio_task_shutdown_pipeline();
            return ret;
        }
    }

    uint8_t asp_status = 0;
    ret = cs43131_read_asp_status(&asp_status); // 清启动阶段 sticky 状态
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }

    for (int i = 0; i < 2; ++i) {
        ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            audio_task_shutdown_pipeline();
            return ret;
        }
    }

    asp_status = 0;
    ret = cs43131_read_asp_status(&asp_status);
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }
    ESP_LOGI(TAG, "WAV起播前 ASP 稳定状态：0x%02X", asp_status);
    if ((asp_status & 0xF8U) != 0) {
        ESP_LOGE(TAG, "ASP 时序异常，拒绝开启耳放：INT_STATUS2=0x%02X", asp_status);
        audio_task_shutdown_pipeline();
        return ESP_FAIL;
    }

    // 预先标记“耳放可能已被触及”，保证寄存器写到一半失败时仍会尝试安全掉电。
    g_pipeline_headphone_enabled = true;
    ret = cs43131_prepare_headphone_playback_low_volume();
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }

    // 耳放 pop-free 上电后继续送约 20ms 静音，再解除 PCM 手动静音。
    for (int i = 0; i < 4; ++i) {
        ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            audio_task_shutdown_pipeline();
            return ret;
        }
    }

    ret = cs43131_set_pcm_mute(false);
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }

    const uint64_t duration_ms = g_task_sample_rate_hz > 0
        ? (g_task_total_frames * 1000ULL) / g_task_sample_rate_hz
        : 0;
    ESP_LOGI(TAG, "WAV正式播放链路已开启：%luHz / %ubit / %u声道，时长约=%llums，PCM音量=-40dB",
        static_cast<unsigned long>(g_task_sample_rate_hz),
        static_cast<unsigned>(g_task_bits_per_sample),
        static_cast<unsigned>(g_task_channels),
        static_cast<unsigned long long>(duration_ms));
    return ESP_OK;
}

static void audio_task_fail_stream(esp_err_t error, const char *stage)
{
    ESP_LOGE(TAG, "WAV播放失败：阶段=%s，错误=%s",
        stage != nullptr ? stage : "未知",
        esp_err_to_name(error));
    esp_err_t cleanup_error = audio_task_shutdown_pipeline();
    if (error == ESP_OK) {
        error = cleanup_error != ESP_OK ? cleanup_error : ESP_FAIL;
    }
    audio_task_set_state(AudioPlaybackState::Error, error);
}

static void audio_task_finish_stream()
{
    g_task_position_frames = g_task_total_frames;
    audio_task_publish_snapshot();

    ESP_LOGI(TAG, "WAV PCM 数据播放完成：帧=%llu/%llu",
        static_cast<unsigned long long>(g_task_position_frames),
        static_cast<unsigned long long>(g_task_total_frames));

    // i2s_channel_write() 返回代表 PCM 已复制进 DMA，不等于最后一个样本已经从引脚送出。
    // 文件尾先追加 4 个静音块，让已排队的最后 PCM 块自然播放完，再执行 DAC 软静音/掉电。
    // 这样不会因为 EOF 清理过快而截掉歌曲最后几毫秒。
    if (g_pipeline_i2s_started && i2s_output_is_started()) {
        esp_err_t drain_ret = i2s_output_stream_write_silence(
            AUDIO_STREAM_FRAMES * 4,
            AUDIO_I2S_WRITE_TIMEOUT_MS
        );
        if (drain_ret != ESP_OK) {
            ESP_LOGE(TAG, "WAV 尾部静音排空失败：%s", esp_err_to_name(drain_ret));
            audio_task_shutdown_pipeline();
            audio_task_set_state(AudioPlaybackState::Error, drain_ret);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_err_t ret = audio_task_shutdown_pipeline();
    if (ret != ESP_OK) {
        audio_task_set_state(AudioPlaybackState::Error, ret);
        return;
    }
    audio_task_set_state(AudioPlaybackState::Finished, ESP_OK);
}

static void audio_task_service_wav_playback()
{
    if (g_task_state != AudioPlaybackState::Playing || !wav_decoder_is_open(&g_wav)) {
        return;
    }

    size_t frames = 0;
    esp_err_t ret = wav_decoder_read_pcm32(
        &g_wav,
        g_pcm_block,
        AUDIO_STREAM_FRAMES,
        &frames
    );
    if (ret != ESP_OK) {
        audio_task_fail_stream(ret, "读取PCM");
        return;
    }

    if (frames == 0) {
        if (wav_decoder_is_eof(&g_wav)) {
            audio_task_finish_stream();
        } else {
            audio_task_fail_stream(ESP_FAIL, "PCM无进度");
        }
        return;
    }

    ret = i2s_output_stream_write_pcm32(g_pcm_block, frames, AUDIO_I2S_WRITE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        audio_task_fail_stream(ret, "I2S发送");
        return;
    }

    g_task_position_frames = g_wav.frames_read;
    const uint64_t publish_interval = g_task_sample_rate_hz >= 4
        ? g_task_sample_rate_hz / 4U
        : 1;
    if (g_task_position_frames - g_last_progress_publish_frame >= publish_interval) {
        g_last_progress_publish_frame = g_task_position_frames;
        audio_task_publish_snapshot();
    }
}

static void audio_task_service_pause_silence()
{
    if (g_task_state != AudioPlaybackState::Paused || !g_pipeline_i2s_started) {
        return;
    }

    // 暂停时不推进文件，但继续送零 PCM，让 CS43131 Slave 始终看到连续 BCLK/LRCK。
    esp_err_t ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        audio_task_fail_stream(ret, "暂停静音");
    }
}

static void audio_task_handle_play(AudioRequest *request)
{
    const char *path = audio_request_path(request);

    // 新播放请求一定先收回上一条播放链路的所有资源。
    esp_err_t cleanup_ret = audio_task_shutdown_pipeline();
    if (cleanup_ret != ESP_OK) {
        g_task_last_request_id = request->request_id;
        audio_task_set_state(AudioPlaybackState::Error, cleanup_ret);
        audio_request_complete(request, false, cleanup_ret);
        return;
    }

    g_task_last_request_id = request->request_id;
    g_task_track_index = request->track_index;
    g_task_format = request->format;
    audio_task_reset_media_fields();
    audio_task_advance_playback_revision();
    audio_task_set_state(AudioPlaybackState::Preparing);

    ESP_LOGI(TAG, "收到播放请求：请求=%lu 世代=%lu 曲目=%lu 格式=%s 路径=%s",
        static_cast<unsigned long>(request->request_id),
        static_cast<unsigned long>(g_task_playback_revision),
        static_cast<unsigned long>(request->track_index + 1),
        media_library_format_name(request->format),
        path != nullptr ? path : "(空)");

    if (path == nullptr || path[0] == '\0') {
        audio_task_set_state(AudioPlaybackState::Error, ESP_ERR_INVALID_ARG);
        audio_request_complete(request, false, ESP_ERR_INVALID_ARG);
        return;
    }

    if (request->format != MediaFormat::WAV) {
        ESP_LOGW(TAG, "Stage 9.2 当前仅接入 WAV PCM；%s 解码器尚未接入",
            media_library_format_name(request->format));
        audio_task_set_state(AudioPlaybackState::Error, ESP_ERR_NOT_SUPPORTED);
        audio_request_complete(request, false, ESP_ERR_NOT_SUPPORTED);
        return;
    }

    esp_err_t ret = audio_task_start_wav_pipeline(path);
    if (ret != ESP_OK) {
        audio_task_set_state(AudioPlaybackState::Error, ret);
        audio_request_complete(request, false, ret);
        return;
    }

    audio_task_set_state(AudioPlaybackState::Playing, ESP_OK);
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_handle_stop(AudioRequest *request)
{
    g_task_last_request_id = request->request_id;
    esp_err_t ret = audio_task_shutdown_pipeline();
    audio_task_advance_playback_revision();
    g_task_track_index = UINT32_MAX;
    g_task_format = MediaFormat::Unknown;
    audio_task_reset_media_fields();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "停止播放时清理音频链路失败：%s", esp_err_to_name(ret));
        audio_task_set_state(AudioPlaybackState::Error, ret);
        audio_request_complete(request, false, ret);
        return;
    }

    audio_task_set_state(AudioPlaybackState::Stopped, ESP_OK);
    ESP_LOGI(TAG, "停止请求完成：请求=%lu，新播放世代=%lu",
        static_cast<unsigned long>(request->request_id),
        static_cast<unsigned long>(g_task_playback_revision));
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_handle_pause(AudioRequest *request)
{
    g_task_last_request_id = request->request_id;
    if (g_task_state != AudioPlaybackState::Playing || !g_pipeline_headphone_enabled) {
        ESP_LOGW(TAG, "暂停请求被拒绝：当前状态=%s",
            audio_playback_state_name_cn(g_task_state));
        audio_task_set_state(g_task_state, ESP_ERR_INVALID_STATE);
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    esp_err_t ret = cs43131_set_pcm_mute(true);
    if (ret != ESP_OK) {
        audio_task_fail_stream(ret, "暂停静音");
        audio_request_complete(request, false, ret);
        return;
    }

    audio_task_set_state(AudioPlaybackState::Paused, ESP_OK);
    ESP_LOGI(TAG, "播放已暂停：帧=%llu/%llu，I2S继续发送静音保持时钟",
        static_cast<unsigned long long>(g_task_position_frames),
        static_cast<unsigned long long>(g_task_total_frames));
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_handle_resume(AudioRequest *request)
{
    g_task_last_request_id = request->request_id;
    if (g_task_state != AudioPlaybackState::Paused || !g_pipeline_headphone_enabled) {
        ESP_LOGW(TAG, "恢复请求被拒绝：当前状态=%s",
            audio_playback_state_name_cn(g_task_state));
        audio_task_set_state(g_task_state, ESP_ERR_INVALID_STATE);
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    esp_err_t ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES * 2, AUDIO_I2S_WRITE_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = cs43131_set_pcm_mute(false);
    }
    if (ret != ESP_OK) {
        audio_task_fail_stream(ret, "恢复播放");
        audio_request_complete(request, false, ret);
        return;
    }

    audio_task_set_state(AudioPlaybackState::Playing, ESP_OK);
    ESP_LOGI(TAG, "播放已恢复：从帧=%llu继续",
        static_cast<unsigned long long>(g_task_position_frames));
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_process_request(AudioRequest *request)
{
    switch (request->type) {
        case AudioCommandType::Play:
            audio_task_handle_play(request);
            break;
        case AudioCommandType::Stop:
            audio_task_handle_stop(request);
            break;
        case AudioCommandType::Pause:
            audio_task_handle_pause(request);
            break;
        case AudioCommandType::Resume:
            audio_task_handle_resume(request);
            break;
    }
}

static void audio_task_main(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "AudioTask 已启动：核心=%d，优先级=%u，栈=%u字节",
        xPortGetCoreID(),
        static_cast<unsigned>(uxTaskPriorityGet(nullptr)),
        static_cast<unsigned>(AUDIO_TASK_STACK_BYTES));

    g_start_result = cs43131_init();
    if (g_start_result == ESP_OK) {
        g_task_ready = true;
        g_task_state = AudioPlaybackState::Ready;
        g_task_last_error = ESP_OK;
        audio_task_publish_snapshot();
        ESP_LOGI(TAG, "AudioTask 已取得运行期音频硬件所有权；DAC 当前保持安全掉电状态");
    } else {
        g_task_ready = false;
        g_task_state = AudioPlaybackState::Error;
        g_task_last_error = g_start_result;
        audio_task_publish_snapshot();
        ESP_LOGE(TAG, "AudioTask 初始化 CS43131 失败：%s", esp_err_to_name(g_start_result));
    }

    if (g_start_done != nullptr) {
        xSemaphoreGive(g_start_done);
    }

    if (g_start_result != ESP_OK) {
        g_audio_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        AudioRequest *request = nullptr;
        const bool stream_needs_service =
            g_task_state == AudioPlaybackState::Playing ||
            g_task_state == AudioPlaybackState::Paused;
        const TickType_t wait_ticks = stream_needs_service ? 0 : portMAX_DELAY;

        if (xQueueReceive(g_command_queue, &request, wait_ticks) == pdTRUE && request != nullptr) {
            audio_task_process_request(request);
            audio_request_release(request);
        }

        if (g_task_state == AudioPlaybackState::Playing) {
            audio_task_service_wav_playback();
        } else if (g_task_state == AudioPlaybackState::Paused) {
            audio_task_service_pause_silence();
        }
    }
}

static bool audio_service_submit(AudioRequest *request, bool wait)
{
    if (request == nullptr || g_command_queue == nullptr || !audio_service_is_ready()) {
        audio_request_release(request);
        return false;
    }

    // 队列持有独立引用。调用方无论同步还是异步，都可以安全释放自己的引用。
    audio_request_retain(request);
    if (xQueueSend(g_command_queue, &request, AUDIO_QUEUE_SEND_TIMEOUT) != pdTRUE) {
        ESP_LOGE(TAG, "音频命令队列已满：请求=%lu",
            static_cast<unsigned long>(request->request_id));
        audio_request_release(request);
        audio_request_release(request);
        return false;
    }

    if (!wait) {
        audio_request_release(request);
        return true;
    }

    if (request->done == nullptr) {
        ESP_LOGE(TAG, "同步请求缺少完成信号量：请求=%lu",
            static_cast<unsigned long>(request->request_id));
        audio_request_release(request);
        return false;
    }

    const bool completed = xSemaphoreTake(request->done, AUDIO_SYNC_WAIT_TIMEOUT) == pdTRUE;
    const bool success = completed && request->success;
    if (!completed) {
        ESP_LOGE(TAG, "等待音频命令完成超时：请求=%lu",
            static_cast<unsigned long>(request->request_id));
    }
    audio_request_release(request);
    return success;
}

const char *audio_playback_state_name_cn(AudioPlaybackState state)
{
    switch (state) {
        case AudioPlaybackState::Starting: return "启动中";
        case AudioPlaybackState::Ready: return "就绪";
        case AudioPlaybackState::Preparing: return "准备中";
        case AudioPlaybackState::Prepared: return "已准备";
        case AudioPlaybackState::Playing: return "播放中";
        case AudioPlaybackState::Paused: return "已暂停";
        case AudioPlaybackState::Finished: return "播放结束";
        case AudioPlaybackState::Stopped: return "已停止";
        case AudioPlaybackState::Error: return "错误";
        default: return "未知";
    }
}

esp_err_t audio_service_start()
{
    if (g_audio_task != nullptr && audio_service_is_ready()) {
        return ESP_OK;
    }

    if (g_command_queue == nullptr) {
        g_command_queue = xQueueCreate(AUDIO_COMMAND_QUEUE_LENGTH, sizeof(AudioRequest *));
        if (g_command_queue == nullptr) {
            ESP_LOGE(TAG, "创建音频命令队列失败");
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_start_done != nullptr) {
        vSemaphoreDelete(g_start_done);
        g_start_done = nullptr;
    }
    g_start_done = xSemaphoreCreateBinary();
    if (g_start_done == nullptr) {
        ESP_LOGE(TAG, "创建 AudioTask 启动信号量失败");
        return ESP_ERR_NO_MEM;
    }

    g_start_result = ESP_ERR_INVALID_STATE;
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        audio_task_main,
        "AudioTask",
        AUDIO_TASK_STACK_BYTES,
        nullptr,
        AUDIO_TASK_PRIORITY,
        &g_audio_task,
        AUDIO_TASK_CORE
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "创建 AudioTask 失败");
        g_audio_task = nullptr;
        vSemaphoreDelete(g_start_done);
        g_start_done = nullptr;
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(g_start_done, AUDIO_START_WAIT_TIMEOUT) != pdTRUE) {
        ESP_LOGE(TAG, "等待 AudioTask 初始化超时");
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(g_start_done);
    g_start_done = nullptr;
    return g_start_result;
}

bool audio_service_is_ready()
{
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot)) {
        return false;
    }
    return snapshot.ready;
}

bool audio_service_get_snapshot(AudioStateSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return false;
    }
    portENTER_CRITICAL(&g_snapshot_mux);
    *out_snapshot = g_snapshot;
    portEXIT_CRITICAL(&g_snapshot_mux);
    return true;
}

bool audio_service_play_track(
    uint32_t track_index,
    const char *path,
    MediaFormat format,
    bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::Play, wait);
    if (request == nullptr) {
        return false;
    }
    request->track_index = track_index;
    request->format = format;
    if (!audio_request_set_path(request, path)) {
        audio_request_release(request);
        return false;
    }
    return audio_service_submit(request, wait);
}

bool audio_service_stop(bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::Stop, wait);
    return audio_service_submit(request, wait);
}

bool audio_service_pause(bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::Pause, wait);
    return audio_service_submit(request, wait);
}

bool audio_service_resume(bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::Resume, wait);
    return audio_service_submit(request, wait);
}

uint32_t audio_service_playback_revision()
{
    AudioStateSnapshot snapshot = {};
    return audio_service_get_snapshot(&snapshot) ? snapshot.playback_revision : 0;
}
