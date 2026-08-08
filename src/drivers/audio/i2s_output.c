#include "i2s_output.h"

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "board_pins.h"

static const char *TAG = "I2S";
static i2s_chan_handle_t g_tx = NULL;
static TaskHandle_t g_stream_task = NULL;
static SemaphoreHandle_t g_stream_task_done = NULL;
static volatile bool g_streaming = false;
static volatile bool g_test_tone_enabled = false;
static volatile esp_err_t g_stream_error = ESP_OK;
static bool g_started = false;
static uint32_t g_sample_rate_hz = 0;

#define I2S_FRAMES_PER_BLOCK 256
#define I2S_DMA_DESC_NUM 4
#define I2S_DMA_FRAME_NUM I2S_FRAMES_PER_BLOCK
#define I2S_WRITE_TIMEOUT_MS 100
#define I2S_MAX_ZERO_PROGRESS_TIMEOUTS 3
#define I2S_TASK_STOP_TIMEOUT_MS 500

// 每帧两个 32bit 声道。
static uint32_t g_silence[I2S_FRAMES_PER_BLOCK * 2] = {0};
static int32_t g_test_tone[I2S_FRAMES_PER_BLOCK * 2] = {0};

// 48kHz 下 1kHz 正好每周期 48 个采样。Q15 正弦表再缩小到约 1/8 满幅，
// 仅供 Stage 8.x 硬件诊断接口使用。
static const int16_t g_sine_q15[48] = {
    0, 4277, 8481, 12539, 16384, 19947, 23170, 25996,
    28378, 30273, 31650, 32487, 32767, 32487, 31650, 30273,
    28378, 25996, 23170, 19947, 16384, 12539, 8481, 4277,
    0, -4277, -8481, -12539, -16384, -19947, -23170, -25996,
    -28378, -30273, -31650, -32487, -32767, -32487, -31650, -30273,
    -28378, -25996, -23170, -19947, -16384, -12539, -8481, -4277
};

static void i2s_build_test_tone(void)
{
    for (int frame = 0; frame < I2S_FRAMES_PER_BLOCK; ++frame) {
        int32_t sample = (int32_t)g_sine_q15[frame % 48] * 8192;
        g_test_tone[frame * 2] = sample;
        g_test_tone[frame * 2 + 1] = sample;
    }
}

static esp_err_t i2s_output_create_channel(uint32_t sample_rate_hz)
{
    if (g_started) {
        return g_sample_rate_hz == sample_rate_hz ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (sample_rate_hz != 44100 && sample_rate_hz != 48000) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "正在初始化 I2S TX：%luHz / 32bit slot / 立体声 / Philips I2S",
        (unsigned long)sample_rate_hz);
    ESP_LOGI(TAG, "BCLK=GPIO%d LRCK=GPIO%d DOUT=GPIO%d，MCLK不从ESP32输出",
        FAKEPOD_I2S_BCLK, FAKEPOD_I2S_LRCK, FAKEPOD_I2S_DOUT);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &g_tx, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 I2S TX 通道失败：%s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_STEREO
        ),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = FAKEPOD_I2S_BCLK,
            .ws = FAKEPOD_I2S_LRCK,
            .dout = FAKEPOD_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(g_tx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化 I2S 标准模式失败：%s", esp_err_to_name(ret));
        i2s_del_channel(g_tx);
        g_tx = NULL;
        return ret;
    }

    ret = i2s_channel_enable(g_tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启用 I2S TX 失败：%s", esp_err_to_name(ret));
        i2s_del_channel(g_tx);
        g_tx = NULL;
        return ret;
    }

    g_sample_rate_hz = sample_rate_hz;
    g_started = true;

    const uint32_t bclk_hz = sample_rate_hz * 64U;
    ESP_LOGI(TAG, "I2S TX 初始化成功：BCLK=%luHz，LRCK=%luHz",
        (unsigned long)bclk_hz,
        (unsigned long)sample_rate_hz);
    ESP_LOGI(TAG, "DMA配置：%d个描述符 × %d帧，单缓冲=%u字节",
        I2S_DMA_DESC_NUM,
        I2S_DMA_FRAME_NUM,
        (unsigned)sizeof(g_silence));
    return ESP_OK;
}

static esp_err_t i2s_output_write_all(const void *data, size_t bytes, uint32_t timeout_ms)
{
    if (!g_started || g_tx == NULL || data == NULL || bytes == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t *cursor = (const uint8_t *)data;
    size_t remaining = bytes;
    int zero_progress_timeouts = 0;

    while (remaining > 0) {
        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(
            g_tx,
            cursor,
            remaining,
            &bytes_written,
            timeout_ms
        );

        if (bytes_written > 0) {
            cursor += bytes_written;
            remaining -= bytes_written;
            zero_progress_timeouts = 0;
        }

        if (ret == ESP_OK) {
            continue;
        }

        // ESP-IDF 超时时可能已经完成部分 DMA 拷贝。
        // 有进度就继续发送剩余数据，不能重复整块 PCM。
        if (ret == ESP_ERR_TIMEOUT && bytes_written > 0) {
            continue;
        }

        if (ret == ESP_ERR_TIMEOUT) {
            zero_progress_timeouts++;
            if (zero_progress_timeouts < I2S_MAX_ZERO_PROGRESS_TIMEOUTS) {
                continue;
            }
        }

        ESP_LOGE(TAG, "PCM写入失败：ret=%s，剩余=%u/%u，连续无进度超时=%d",
            esp_err_to_name(ret),
            (unsigned)remaining,
            (unsigned)bytes,
            zero_progress_timeouts);
        return ret != ESP_OK ? ret : ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t i2s_output_stream_start_32bit(uint32_t sample_rate_hz)
{
    if (g_stream_task != NULL || g_streaming) {
        ESP_LOGE(TAG, "旧硬件自检发送任务仍在运行，不能启动正式 PCM 流");
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_output_create_channel(sample_rate_hz);
}

esp_err_t i2s_output_stream_write_pcm32(
    const int32_t *interleaved_stereo,
    size_t frames,
    uint32_t timeout_ms)
{
    if (interleaved_stereo == NULL || frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (frames > SIZE_MAX / (2U * sizeof(int32_t))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return i2s_output_write_all(
        interleaved_stereo,
        frames * 2U * sizeof(int32_t),
        timeout_ms
    );
}

esp_err_t i2s_output_stream_write_silence(size_t frames, uint32_t timeout_ms)
{
    if (frames == 0) {
        return ESP_OK;
    }

    while (frames > 0) {
        size_t chunk = frames > I2S_FRAMES_PER_BLOCK ? I2S_FRAMES_PER_BLOCK : frames;
        esp_err_t ret = i2s_output_write_all(
            g_silence,
            chunk * 2U * sizeof(uint32_t),
            timeout_ms
        );
        if (ret != ESP_OK) {
            return ret;
        }
        frames -= chunk;
    }
    return ESP_OK;
}

static void i2s_silence_task(void *arg)
{
    (void)arg;
    const size_t block_size = sizeof(g_silence);

    while (g_streaming) {
        const void *active_block = g_test_tone_enabled ? (const void *)g_test_tone : (const void *)g_silence;
        const uint8_t *cursor = (const uint8_t *)active_block;
        size_t remaining = block_size;
        int zero_progress_timeouts = 0;

        while (g_streaming && remaining > 0) {
            size_t bytes_written = 0;
            esp_err_t ret = i2s_channel_write(
                g_tx,
                cursor,
                remaining,
                &bytes_written,
                I2S_WRITE_TIMEOUT_MS
            );

            if (bytes_written > 0) {
                cursor += bytes_written;
                remaining -= bytes_written;
                zero_progress_timeouts = 0;
            }
            if (ret == ESP_OK) {
                continue;
            }
            if (ret == ESP_ERR_TIMEOUT && bytes_written > 0) {
                continue;
            }
            if (ret == ESP_ERR_TIMEOUT) {
                zero_progress_timeouts++;
                if (zero_progress_timeouts < I2S_MAX_ZERO_PROGRESS_TIMEOUTS) {
                    continue;
                }
            }

            g_stream_error = ret != ESP_OK ? ret : ESP_FAIL;
            ESP_LOGE(TAG, "后台自检 PCM 发送失败：ret=%s，剩余=%u/%u，连续无进度超时=%d",
                esp_err_to_name(ret),
                (unsigned)remaining,
                (unsigned)block_size,
                zero_progress_timeouts);
            g_streaming = false;
            break;
        }
    }

    g_streaming = false;
    g_stream_task = NULL;
    if (g_stream_task_done != NULL) {
        xSemaphoreGive(g_stream_task_done);
    }
    vTaskDelete(NULL);
}

esp_err_t i2s_output_start_48k_32bit(void)
{
    esp_err_t ret = i2s_output_create_channel(48000);
    if (ret != ESP_OK) {
        return ret;
    }

    i2s_build_test_tone();
    g_test_tone_enabled = false;
    g_stream_error = ESP_OK;
    g_streaming = true;

    if (g_stream_task_done != NULL) {
        vSemaphoreDelete(g_stream_task_done);
        g_stream_task_done = NULL;
    }
    g_stream_task_done = xSemaphoreCreateBinary();
    if (g_stream_task_done == NULL) {
        ESP_LOGE(TAG, "创建 I2S 自检任务退出信号量失败");
        g_streaming = false;
        i2s_output_stop();
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ret = xTaskCreate(
        i2s_silence_task,
        "i2s_silence",
        3072,
        NULL,
        5,
        &g_stream_task
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "创建 I2S 自检发送任务失败");
        g_streaming = false;
        vSemaphoreDelete(g_stream_task_done);
        g_stream_task_done = NULL;
        i2s_output_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t i2s_output_wait_silence_ms(uint32_t duration_ms)
{
    if (!g_started || g_tx == NULL || !g_streaming) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t duration = pdMS_TO_TICKS(duration_ms);
    while ((xTaskGetTickCount() - start) < duration) {
        if (g_stream_error != ESP_OK || !g_streaming) {
            return g_stream_error != ESP_OK ? g_stream_error : ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return g_stream_error;
}

void i2s_output_set_test_tone(bool enabled)
{
    g_test_tone_enabled = enabled;
    ESP_LOGI(TAG, "1kHz低电平测试音：%s", enabled ? "开启" : "关闭");
}

esp_err_t i2s_output_stop(void)
{
    g_test_tone_enabled = false;
    g_streaming = false;

    if (g_tx == NULL) {
        g_started = false;
        g_sample_rate_hz = 0;
        g_stream_task = NULL;
        if (g_stream_task_done != NULL) {
            vSemaphoreDelete(g_stream_task_done);
            g_stream_task_done = NULL;
        }
        return ESP_OK;
    }

    // 旧自检模式如果存在后台任务，必须让任务主动退出后才能释放 channel。
    if (g_stream_task_done != NULL) {
        if (xSemaphoreTake(
                g_stream_task_done,
                pdMS_TO_TICKS(I2S_TASK_STOP_TIMEOUT_MS)
            ) != pdTRUE) {
            ESP_LOGE(TAG, "等待 I2S 发送任务自行退出超时：%dms；不强制删除任务或通道",
                I2S_TASK_STOP_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGI(TAG, "I2S 发送任务已正常退出");
        vSemaphoreDelete(g_stream_task_done);
        g_stream_task_done = NULL;
    }

    esp_err_t ret = i2s_channel_disable(g_tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "关闭 I2S TX 通道失败：%s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_del_channel(g_tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "删除 I2S TX 通道失败：%s", esp_err_to_name(ret));
        return ret;
    }

    g_tx = NULL;
    g_stream_task = NULL;
    g_started = false;
    g_sample_rate_hz = 0;
    ESP_LOGI(TAG, "I2S TX 已正常停止并释放");
    return ESP_OK;
}

bool i2s_output_is_started(void)
{
    return g_started;
}

uint32_t i2s_output_sample_rate_hz(void)
{
    return g_sample_rate_hz;
}
