#include "avi_mp3_audio_source.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace
{

static portMUX_TYPE g_bridge_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *g_buffer = nullptr;
static size_t g_capacity = 0U;
static size_t g_read_pos = 0U;
static size_t g_write_pos = 0U;
static size_t g_buffered = 0U;
static size_t g_high_water = 0U;
static bool g_active = false;
static bool g_eof = false;
static bool g_cancelled = false;
static bool g_source_open = false;
static uint64_t g_bytes_pushed = 0ULL;
static uint64_t g_bytes_read = 0ULL;
static uint32_t g_push_wait_count = 0U;
static uint32_t g_read_wait_count = 0U;
static uint64_t g_read_wait_us_total = 0ULL;
static uint32_t g_read_wait_us_max = 0U;
static uint32_t g_read_wait_over_1ms = 0U;
static uint32_t g_read_wait_over_5ms = 0U;
static uint32_t g_read_wait_over_10ms = 0U;
static uint32_t g_read_wait_timeout_count = 0U;
static SemaphoreHandle_t g_data_event = nullptr;
static SemaphoreHandle_t g_space_event = nullptr;

static void bridge_signal_all()
{
    if (g_data_event != nullptr) xSemaphoreGive(g_data_event);
    if (g_space_event != nullptr) xSemaphoreGive(g_space_event);
}

static size_t bridge_copy_out(void *buffer, size_t bytes)
{
    if (buffer == nullptr || bytes == 0U) return 0U;
    size_t copied = 0U;
    portENTER_CRITICAL(&g_bridge_mux);
    if (g_active && g_buffer != nullptr && g_buffered != 0U) {
        copied = bytes < g_buffered ? bytes : g_buffered;
        const size_t first = copied < (g_capacity - g_read_pos)
            ? copied : (g_capacity - g_read_pos);
        memcpy(buffer, g_buffer + g_read_pos, first);
        if (copied > first) {
            memcpy(static_cast<uint8_t *>(buffer) + first, g_buffer, copied - first);
        }
        g_read_pos = (g_read_pos + copied) % g_capacity;
        g_buffered -= copied;
        g_bytes_read += copied;
    }
    portEXIT_CRITICAL(&g_bridge_mux);
    if (copied != 0U && g_space_event != nullptr) xSemaphoreGive(g_space_event);
    return copied;
}

static esp_err_t bridge_source_read(void *, void *buffer, size_t bytes, size_t *out_bytes)
{
    if (buffer == nullptr || out_bytes == nullptr) return ESP_ERR_INVALID_ARG;
    *out_bytes = 0U;

    while (true) {
        const size_t copied = bridge_copy_out(buffer, bytes);
        if (copied != 0U) {
            *out_bytes = copied;
            return ESP_OK;
        }

        bool terminal = false;
        portENTER_CRITICAL(&g_bridge_mux);
        terminal = !g_active || g_cancelled || (g_eof && g_buffered == 0U);
        if (!terminal) ++g_read_wait_count;
        portEXIT_CRITICAL(&g_bridge_mux);
        if (terminal) return ESP_OK;

        if (g_data_event == nullptr) return ESP_ERR_INVALID_STATE;
        const int64_t wait_started_us = esp_timer_get_time();
        const BaseType_t signalled = xSemaphoreTake(g_data_event, pdMS_TO_TICKS(20));
        const int64_t waited_signed_us = esp_timer_get_time() - wait_started_us;
        const uint32_t waited_us = waited_signed_us <= 0LL
            ? 0U
            : (waited_signed_us > static_cast<int64_t>(UINT32_MAX)
                ? UINT32_MAX : static_cast<uint32_t>(waited_signed_us));
        portENTER_CRITICAL(&g_bridge_mux);
        g_read_wait_us_total += waited_us;
        if (waited_us > g_read_wait_us_max) g_read_wait_us_max = waited_us;
        if (waited_us >= 1000U) ++g_read_wait_over_1ms;
        if (waited_us >= 5000U) ++g_read_wait_over_5ms;
        if (waited_us >= 10000U) ++g_read_wait_over_10ms;
        if (signalled != pdTRUE) ++g_read_wait_timeout_count;
        portEXIT_CRITICAL(&g_bridge_mux);
    }
}

static bool bridge_source_eof(void *)
{
    bool eof = true;
    portENTER_CRITICAL(&g_bridge_mux);
    eof = !g_active || g_cancelled || (g_eof && g_buffered == 0U);
    portEXIT_CRITICAL(&g_bridge_mux);
    return eof;
}

static esp_err_t bridge_source_close(void *context)
{
    AviMp3AudioSource *source = static_cast<AviMp3AudioSource *>(context);
    if (source == nullptr) return ESP_ERR_INVALID_ARG;
    source->open = false;
    portENTER_CRITICAL(&g_bridge_mux);
    g_source_open = false;
    portEXIT_CRITICAL(&g_bridge_mux);
    bridge_signal_all();
    return ESP_OK;
}

static const char *bridge_source_name(void *)
{
    return "AVI_MP3_STREAM";
}

static const AudioSourceOps AVI_MP3_SOURCE_OPS = {
    bridge_source_read,
    nullptr,
    nullptr,
    nullptr,
    bridge_source_eof,
    bridge_source_close,
    bridge_source_name,
};

} // namespace

esp_err_t avi_mp3_bridge_begin(size_t capacity_bytes)
{
    if (capacity_bytes < 4096U) return ESP_ERR_INVALID_SIZE;

    portENTER_CRITICAL(&g_bridge_mux);
    const bool busy = g_source_open;
    portEXIT_CRITICAL(&g_bridge_mux);
    if (busy) return ESP_ERR_INVALID_STATE;

    if (g_buffer != nullptr) {
        heap_caps_free(g_buffer);
        g_buffer = nullptr;
    }
    if (g_data_event != nullptr) {
        vSemaphoreDelete(g_data_event);
        g_data_event = nullptr;
    }
    if (g_space_event != nullptr) {
        vSemaphoreDelete(g_space_event);
        g_space_event = nullptr;
    }

    uint8_t *buffer = static_cast<uint8_t *>(
        heap_caps_malloc(capacity_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) return ESP_ERR_NO_MEM;
    SemaphoreHandle_t data_event = xSemaphoreCreateBinary();
    SemaphoreHandle_t space_event = xSemaphoreCreateBinary();
    if (data_event == nullptr || space_event == nullptr) {
        if (data_event != nullptr) vSemaphoreDelete(data_event);
        if (space_event != nullptr) vSemaphoreDelete(space_event);
        heap_caps_free(buffer);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&g_bridge_mux);
    g_buffer = buffer;
    g_capacity = capacity_bytes;
    g_read_pos = 0U;
    g_write_pos = 0U;
    g_buffered = 0U;
    g_high_water = 0U;
    g_active = true;
    g_eof = false;
    g_cancelled = false;
    g_source_open = false;
    g_bytes_pushed = 0ULL;
    g_bytes_read = 0ULL;
    g_push_wait_count = 0U;
    g_read_wait_count = 0U;
    g_read_wait_us_total = 0ULL;
    g_read_wait_us_max = 0U;
    g_read_wait_over_1ms = 0U;
    g_read_wait_over_5ms = 0U;
    g_read_wait_over_10ms = 0U;
    g_read_wait_timeout_count = 0U;
    g_data_event = data_event;
    g_space_event = space_event;
    portEXIT_CRITICAL(&g_bridge_mux);
    return ESP_OK;
}

esp_err_t avi_mp3_bridge_push(const void *data, size_t bytes, uint32_t timeout_ms)
{
    if (data == nullptr || bytes == 0U) return ESP_ERR_INVALID_ARG;
    const uint8_t *src = static_cast<const uint8_t *>(data);
    size_t offset = 0U;
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms != 0U && timeout_ticks == 0) timeout_ticks = 1;
    const TickType_t started = xTaskGetTickCount();

    while (offset < bytes) {
        size_t copied = 0U;
        bool active = false;
        portENTER_CRITICAL(&g_bridge_mux);
        active = g_active && !g_cancelled && !g_eof && g_buffer != nullptr;
        if (active && g_buffered < g_capacity) {
            const size_t free_bytes = g_capacity - g_buffered;
            copied = (bytes - offset) < free_bytes ? (bytes - offset) : free_bytes;
            const size_t first = copied < (g_capacity - g_write_pos)
                ? copied : (g_capacity - g_write_pos);
            memcpy(g_buffer + g_write_pos, src + offset, first);
            if (copied > first) {
                memcpy(g_buffer, src + offset + first, copied - first);
            }
            g_write_pos = (g_write_pos + copied) % g_capacity;
            g_buffered += copied;
            g_bytes_pushed += copied;
            if (g_buffered > g_high_water) g_high_water = g_buffered;
        }
        portEXIT_CRITICAL(&g_bridge_mux);

        if (!active) return ESP_ERR_INVALID_STATE;
        if (copied != 0U) {
            offset += copied;
            if (g_data_event != nullptr) xSemaphoreGive(g_data_event);
            continue;
        }

        portENTER_CRITICAL(&g_bridge_mux);
        ++g_push_wait_count;
        portEXIT_CRITICAL(&g_bridge_mux);
        if (timeout_ticks == 0) return ESP_ERR_TIMEOUT;
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout_ticks) return ESP_ERR_TIMEOUT;
        TickType_t wait_ticks = timeout_ticks - elapsed;
        const TickType_t slice = pdMS_TO_TICKS(20);
        if (slice != 0 && wait_ticks > slice) wait_ticks = slice;
        if (g_space_event == nullptr) return ESP_ERR_INVALID_STATE;
        (void)xSemaphoreTake(g_space_event, wait_ticks);
    }
    return ESP_OK;
}

void avi_mp3_bridge_mark_eof()
{
    portENTER_CRITICAL(&g_bridge_mux);
    if (g_active && !g_cancelled) g_eof = true;
    portEXIT_CRITICAL(&g_bridge_mux);
    bridge_signal_all();
}

void avi_mp3_bridge_cancel()
{
    portENTER_CRITICAL(&g_bridge_mux);
    if (g_active) g_cancelled = true;
    portEXIT_CRITICAL(&g_bridge_mux);
    bridge_signal_all();
}

esp_err_t avi_mp3_bridge_release()
{
    portENTER_CRITICAL(&g_bridge_mux);
    const bool busy = g_source_open;
    portEXIT_CRITICAL(&g_bridge_mux);
    if (busy) return ESP_ERR_INVALID_STATE;

    if (g_buffer != nullptr) {
        heap_caps_free(g_buffer);
        g_buffer = nullptr;
    }
    if (g_data_event != nullptr) {
        vSemaphoreDelete(g_data_event);
        g_data_event = nullptr;
    }
    if (g_space_event != nullptr) {
        vSemaphoreDelete(g_space_event);
        g_space_event = nullptr;
    }

    portENTER_CRITICAL(&g_bridge_mux);
    g_capacity = 0U;
    g_read_pos = 0U;
    g_write_pos = 0U;
    g_buffered = 0U;
    g_high_water = 0U;
    g_active = false;
    g_eof = false;
    g_cancelled = false;
    g_bytes_pushed = 0ULL;
    g_bytes_read = 0ULL;
    g_push_wait_count = 0U;
    g_read_wait_count = 0U;
    g_read_wait_us_total = 0ULL;
    g_read_wait_us_max = 0U;
    g_read_wait_over_1ms = 0U;
    g_read_wait_over_5ms = 0U;
    g_read_wait_over_10ms = 0U;
    g_read_wait_timeout_count = 0U;
    portEXIT_CRITICAL(&g_bridge_mux);
    return ESP_OK;
}

bool avi_mp3_bridge_get_snapshot(AviMp3BridgeSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    portENTER_CRITICAL(&g_bridge_mux);
    out_snapshot->active = g_active;
    out_snapshot->eof = g_eof;
    out_snapshot->cancelled = g_cancelled;
    out_snapshot->buffered_bytes = g_buffered;
    out_snapshot->capacity_bytes = g_capacity;
    out_snapshot->high_water_bytes = g_high_water;
    out_snapshot->bytes_pushed = g_bytes_pushed;
    out_snapshot->bytes_read = g_bytes_read;
    out_snapshot->push_wait_count = g_push_wait_count;
    out_snapshot->read_wait_count = g_read_wait_count;
    out_snapshot->read_wait_us_total = g_read_wait_us_total;
    out_snapshot->read_wait_us_max = g_read_wait_us_max;
    out_snapshot->read_wait_over_1ms = g_read_wait_over_1ms;
    out_snapshot->read_wait_over_5ms = g_read_wait_over_5ms;
    out_snapshot->read_wait_over_10ms = g_read_wait_over_10ms;
    out_snapshot->read_wait_timeout_count = g_read_wait_timeout_count;
    portEXIT_CRITICAL(&g_bridge_mux);
    return true;
}

esp_err_t avi_mp3_audio_source_open(AudioSource *out_source, AviMp3AudioSource *storage)
{
    if (out_source == nullptr || storage == nullptr) return ESP_ERR_INVALID_ARG;
    audio_source_close(out_source);

    bool ready = false;
    portENTER_CRITICAL(&g_bridge_mux);
    ready = g_active && !g_cancelled && g_buffer != nullptr && !g_source_open;
    if (ready) g_source_open = true;
    portEXIT_CRITICAL(&g_bridge_mux);
    if (!ready) return ESP_ERR_INVALID_STATE;

    *storage = {};
    storage->open = true;
    out_source->ops = &AVI_MP3_SOURCE_OPS;
    out_source->context = storage;
    out_source->capabilities =
        AUDIO_SOURCE_CAP_READ |
        AUDIO_SOURCE_CAP_EOF |
        AUDIO_SOURCE_CAP_STREAMING;
    out_source->stats = {};
    return ESP_OK;
}
