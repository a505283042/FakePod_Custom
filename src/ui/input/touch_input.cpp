#include "touch_input.h"

#include "cst820.h"
#include "board_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace
{
static const char *TAG = "触摸快路";

constexpr BaseType_t TOUCH_INPUT_TASK_CORE = 1;
constexpr UBaseType_t TOUCH_INPUT_TASK_PRIORITY = 3;
constexpr uint32_t TOUCH_INPUT_TASK_STACK_BYTES = 3072U;
constexpr UBaseType_t TOUCH_EDGE_QUEUE_LENGTH = 8U;
constexpr uint32_t TOUCH_SAMPLE_PERIOD_MS = 8U;
constexpr TickType_t TOUCH_SAMPLE_PERIOD_TICKS =
    pdMS_TO_TICKS(TOUCH_SAMPLE_PERIOD_MS) > 0 ? pdMS_TO_TICKS(TOUCH_SAMPLE_PERIOD_MS) : 1U;

TaskHandle_t g_task = nullptr;
QueueHandle_t g_edge_queue = nullptr;
portMUX_TYPE g_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
UiTouchSnapshot g_snapshot = {};
bool g_ready = false;
uint32_t g_last_activity_tick = 0U;
uint32_t g_edge_drop_count = 0U;

static int16_t touch_clamp_coord(uint16_t value, uint16_t max_value)
{
    return static_cast<int16_t>(value > max_value ? max_value : value);
}

static void touch_publish_snapshot(const CST820Point &point, uint32_t tick_ms, bool edge)
{
    bool previous_pressed = false;
    int16_t previous_x = 0;
    int16_t previous_y = 0;
    uint32_t sequence = 0U;
    const int16_t next_x = point.pressed
        ? touch_clamp_coord(point.x, FAKEPOD_LCD_WIDTH - 1U)
        : 0;
    const int16_t next_y = point.pressed
        ? touch_clamp_coord(point.y, FAKEPOD_LCD_HEIGHT - 1U)
        : 0;

    portENTER_CRITICAL(&g_snapshot_mux);
    previous_pressed = g_snapshot.pressed;
    previous_x = g_snapshot.x;
    previous_y = g_snapshot.y;

    // P1.5R.1.2：sequence 不再只代表边沿，也代表真实坐标变化。
    // LVGL 可以继续读取当前 pressed/released，但业务层只在 sequence 变化时收到一次输入。
    const bool moved = point.pressed &&
        (next_x != previous_x || next_y != previous_y);
    if (edge || moved) {
        ++g_snapshot.sequence;
        if (g_snapshot.sequence == 0U) {
            ++g_snapshot.sequence;
        }
    }
    sequence = g_snapshot.sequence;
    g_snapshot.pressed = point.pressed;
    if (point.pressed) {
        g_snapshot.x = next_x;
        g_snapshot.y = next_y;
    } else {
        // CST820 在 RELEASED 时不再提供有效坐标；沿用最后一个按下坐标。
        g_snapshot.x = previous_x;
        g_snapshot.y = previous_y;
    }
    g_snapshot.tick_ms = tick_ms;
    if (point.pressed || previous_pressed != point.pressed) {
        g_last_activity_tick = tick_ms;
    }
    portEXIT_CRITICAL(&g_snapshot_mux);

    if (!edge || g_edge_queue == nullptr) {
        return;
    }

    UiTouchEdgeEvent event = {};
    event.pressed = point.pressed;
    event.x = point.pressed ? touch_clamp_coord(point.x, FAKEPOD_LCD_WIDTH - 1U) : previous_x;
    event.y = point.pressed ? touch_clamp_coord(point.y, FAKEPOD_LCD_HEIGHT - 1U) : previous_y;
    event.tick_ms = tick_ms;
    event.sequence = sequence;

    if (xQueueSend(g_edge_queue, &event, 0) != pdPASS) {
        ++g_edge_drop_count;
        if (g_edge_drop_count == 1U || (g_edge_drop_count % 16U) == 0U) {
            ESP_LOGW(TAG, "触摸边沿队列已满：drop=%lu", static_cast<unsigned long>(g_edge_drop_count));
        }
    }
}

static void touch_input_task(void *argument)
{
    (void)argument;
    TickType_t last_wake = xTaskGetTickCount();
    bool last_pressed = false;
    uint32_t read_error_count = 0U;

    for (;;) {
        CST820Point point = {};
        const esp_err_t ret = cst820_read_point(&point);
        if (ret == ESP_OK) {
            const uint32_t now_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
            const bool edge = point.pressed != last_pressed;
            touch_publish_snapshot(point, now_ms, edge);
            last_pressed = point.pressed;
            read_error_count = 0U;
        } else {
            // 短暂 I2C 错误时保留最近状态，不伪造 RELEASED，避免误触/误点击。
            ++read_error_count;
            if (read_error_count == 1U || (read_error_count % 100U) == 0U) {
                ESP_LOGW(TAG, "CST820采样失败：%s count=%lu",
                    esp_err_to_name(ret), static_cast<unsigned long>(read_error_count));
            }
        }

        vTaskDelayUntil(&last_wake, TOUCH_SAMPLE_PERIOD_TICKS);
    }
}
} // namespace

esp_err_t ui_touch_input_start()
{
    if (g_ready && g_task != nullptr) {
        return ESP_OK;
    }
    if (!cst820_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (g_edge_queue == nullptr) {
        g_edge_queue = xQueueCreate(TOUCH_EDGE_QUEUE_LENGTH, sizeof(UiTouchEdgeEvent));
        if (g_edge_queue == nullptr) {
            ESP_LOGE(TAG, "创建触摸边沿队列失败");
            return ESP_ERR_NO_MEM;
        }
    } else {
        xQueueReset(g_edge_queue);
    }

    portENTER_CRITICAL(&g_snapshot_mux);
    g_snapshot = {};
    g_last_activity_tick = 0U;
    portEXIT_CRITICAL(&g_snapshot_mux);
    g_edge_drop_count = 0U;

    const BaseType_t created = xTaskCreatePinnedToCore(
        touch_input_task,
        "TouchInput",
        TOUCH_INPUT_TASK_STACK_BYTES,
        nullptr,
        TOUCH_INPUT_TASK_PRIORITY,
        &g_task,
        TOUCH_INPUT_TASK_CORE);
    if (created != pdPASS) {
        g_task = nullptr;
        ESP_LOGE(TAG, "创建 TouchInputTask 失败");
        return ESP_ERR_NO_MEM;
    }

    g_ready = true;
    ESP_LOGI(TAG,
        "P1.5R.1.2 Touch Fast Path/QoS：core=%ld priority=%u poll=%ums/%utick edge_queue=%u；坐标变化sequence去重；LVGL回调不再访问I2C",
        static_cast<long>(TOUCH_INPUT_TASK_CORE),
        static_cast<unsigned>(TOUCH_INPUT_TASK_PRIORITY),
        static_cast<unsigned>(TOUCH_SAMPLE_PERIOD_MS),
        static_cast<unsigned>(TOUCH_SAMPLE_PERIOD_TICKS),
        static_cast<unsigned>(TOUCH_EDGE_QUEUE_LENGTH));
    return ESP_OK;
}

bool ui_touch_input_is_ready()
{
    return g_ready && g_task != nullptr && g_edge_queue != nullptr;
}

bool ui_touch_input_get_snapshot(UiTouchSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr || !ui_touch_input_is_ready()) {
        return false;
    }
    portENTER_CRITICAL(&g_snapshot_mux);
    *out_snapshot = g_snapshot;
    portEXIT_CRITICAL(&g_snapshot_mux);
    return true;
}

bool ui_touch_input_take_edge(UiTouchEdgeEvent *out_event)
{
    if (out_event == nullptr || !ui_touch_input_is_ready()) {
        return false;
    }
    return xQueueReceive(g_edge_queue, out_event, 0) == pdPASS;
}

bool ui_touch_input_has_pending_edge()
{
    return ui_touch_input_is_ready() && uxQueueMessagesWaiting(g_edge_queue) > 0U;
}

bool ui_touch_input_recent_activity(uint32_t activity_window_ms)
{
    if (!ui_touch_input_is_ready()) {
        return false;
    }

    bool pressed = false;
    uint32_t last_activity_tick = 0U;
    portENTER_CRITICAL(&g_snapshot_mux);
    pressed = g_snapshot.pressed;
    last_activity_tick = g_last_activity_tick;
    portEXIT_CRITICAL(&g_snapshot_mux);

    if (pressed) {
        return true;
    }
    if (last_activity_tick == 0U) {
        return false;
    }

    const uint32_t now_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return static_cast<uint32_t>(now_ms - last_activity_tick) <= activity_window_ms;
}
