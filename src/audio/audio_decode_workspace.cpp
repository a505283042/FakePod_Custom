#include "audio_decode_workspace.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "解码工作区";

static esp_err_t audio_decode_workspace_reserve(
    uint8_t **buffer,
    size_t *capacity,
    size_t required_bytes,
    uint8_t **out_buffer,
    const char *name)
{
    if (
        buffer == nullptr ||
        capacity == nullptr ||
        out_buffer == nullptr ||
        required_bytes == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (*buffer != nullptr && *capacity >= required_bytes) {
        *out_buffer = *buffer;
        return ESP_OK;
    }

    uint8_t *replacement = static_cast<uint8_t *>(
        heap_caps_malloc(required_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (replacement == nullptr) {
        ESP_LOGE(TAG, "%s PSRAM 分配失败：required=%uB，禁止回落内部RAM",
            name != nullptr ? name : "workspace",
            static_cast<unsigned>(required_bytes));
        return ESP_ERR_NO_MEM;
    }

    if (*buffer != nullptr) {
        heap_caps_free(*buffer);
    }
    *buffer = replacement;
    *capacity = required_bytes;
    *out_buffer = replacement;

    ESP_LOGI(TAG, "%s 扩容：capacity=%uB",
        name != nullptr ? name : "workspace",
        static_cast<unsigned>(required_bytes));
    return ESP_OK;
}

esp_err_t audio_decode_workspace_reserve_input(
    AudioDecodeWorkspace *workspace,
    size_t required_bytes,
    uint8_t **out_buffer)
{
    if (workspace == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return audio_decode_workspace_reserve(
        &workspace->input,
        &workspace->input_capacity,
        required_bytes,
        out_buffer,
        "压缩输入工作区");
}

esp_err_t audio_decode_workspace_reserve_decoded(
    AudioDecodeWorkspace *workspace,
    size_t required_bytes,
    uint8_t **out_buffer)
{
    if (workspace == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return audio_decode_workspace_reserve(
        &workspace->decoded,
        &workspace->decoded_capacity,
        required_bytes,
        out_buffer,
        "解码PCM工作区");
}

void audio_decode_workspace_trim(
    AudioDecodeWorkspace *workspace,
    size_t max_retained_input_bytes,
    size_t max_retained_decoded_bytes)
{
    if (workspace == nullptr) {
        return;
    }

    if (workspace->input != nullptr && workspace->input_capacity > max_retained_input_bytes) {
        ESP_LOGI(TAG, "释放异常膨胀输入工作区：%uB > %uB",
            static_cast<unsigned>(workspace->input_capacity),
            static_cast<unsigned>(max_retained_input_bytes));
        heap_caps_free(workspace->input);
        workspace->input = nullptr;
        workspace->input_capacity = 0;
    }
    if (workspace->decoded != nullptr && workspace->decoded_capacity > max_retained_decoded_bytes) {
        ESP_LOGI(TAG, "释放异常膨胀PCM工作区：%uB > %uB",
            static_cast<unsigned>(workspace->decoded_capacity),
            static_cast<unsigned>(max_retained_decoded_bytes));
        heap_caps_free(workspace->decoded);
        workspace->decoded = nullptr;
        workspace->decoded_capacity = 0;
    }
}

void audio_decode_workspace_destroy(AudioDecodeWorkspace *workspace)
{
    if (workspace == nullptr) {
        return;
    }
    if (workspace->input != nullptr) {
        heap_caps_free(workspace->input);
    }
    if (workspace->decoded != nullptr) {
        heap_caps_free(workspace->decoded);
    }
    *workspace = {};
}

size_t audio_decode_workspace_total_bytes(const AudioDecodeWorkspace *workspace)
{
    if (workspace == nullptr) {
        return 0;
    }
    return workspace->input_capacity + workspace->decoded_capacity;
}
