#include "touch_input.h"

#include "cst820.h"
#include "board_pins.h"
#include "esp_log.h"
#include "app_diag_config.h"
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
constexpr uint8_t TOUCH_RELEASE_DEBOUNCE_SAMPLES = 3U;
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
uint32_t g_edge_coalesce_count = 0U;
uint32_t g_edge_coalesced_events = 0U;
uint32_t g_release_glitch_suppressed_count = 0U;

static int16_t touch_clamp_coord(uint16_t value, uint16_t max_value)
{
    return static_cast<int16_t>(value > max_value ? max_value : value);
}

static bool touch_enqueue_edge_with_backpressure(const UiTouchEdgeEvent &event)
{
    if (g_edge_queue == nullptr) {
        return false;
    }

    if (xQueueSend(g_edge_queue, &event, 0) == pdPASS) {
        return true;
    }

    // R.33.2.3：边沿 FIFO 满时不继续堆积过期手势。TouchInputTask 是唯一生产者，
    // 因此可以安全地抽干旧边沿，并优先保留“最近一次 DOWN + 当前 RELEASE”。
    // 这样 UI 长时间被 BoundedSPI 大面积物理提交占用时，宁可合并已经过期的完整点击，也不能
    // 丢掉最新物理状态，尤其不能让 RELEASE 丢失导致 GestureRouter/LVGL 卡在 pressed。
    UiTouchEdgeEvent stale[TOUCH_EDGE_QUEUE_LENGTH] = {};
    UBaseType_t stale_count = 0U;
    while (stale_count < TOUCH_EDGE_QUEUE_LENGTH &&
           xQueueReceive(g_edge_queue, &stale[stale_count], 0) == pdPASS) {
        ++stale_count;
    }

    bool kept_down = false;
    if (!event.pressed) {
        for (UBaseType_t i = stale_count; i > 0U; --i) {
            if (stale[i - 1U].pressed) {
                if (xQueueSend(g_edge_queue, &stale[i - 1U], 0) == pdPASS) {
                    kept_down = true;
                }
                break;
            }
        }
    }

    const bool queued = xQueueSend(g_edge_queue, &event, 0) == pdPASS;
    ++g_edge_coalesce_count;
    g_edge_coalesced_events += static_cast<uint32_t>(stale_count);

    if (g_edge_coalesce_count == 1U || (g_edge_coalesce_count % 8U) == 0U || !queued) {
        ESP_LOGW(TAG,
            "触摸边沿背压：coalesce=%lu stale_total=%lu drained=%u latest=%s keep_down=%u queued=%u",
            static_cast<unsigned long>(g_edge_coalesce_count),
            static_cast<unsigned long>(g_edge_coalesced_events),
            static_cast<unsigned>(stale_count),
            event.pressed ? "DOWN" : "UP",
            static_cast<unsigned>(kept_down),
            static_cast<unsigned>(queued));
    }

    if (queued) {
        return true;
    }

    ++g_edge_drop_count;
    ESP_LOGE(TAG,
        "触摸边沿背压最终入队失败：drop=%lu state=%s",
        static_cast<unsigned long>(g_edge_drop_count),
        event.pressed ? "DOWN" : "UP");
    return false;
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

    (void)touch_enqueue_edge_with_backpressure(event);
}

static void touch_input_task(void *argument)
{
    (void)argument;
    TickType_t last_wake = xTaskGetTickCount();
    bool stable_pressed = false;
    uint8_t release_candidate_samples = 0U;
    uint32_t read_error_count = 0U;

    for (;;) {
        CST820Point point = {};
        const esp_err_t ret = cst820_read_point(&point);
        if (ret == ESP_OK) {
            const uint32_t now_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
            read_error_count = 0U;

            if (point.pressed) {
                if (release_candidate_samples > 0U) {
                    // CST820 在手指移动/BoundedSPI 重负载期间偶尔会短暂报告 0 指。
                    // 只要在确认 RELEASE 前又恢复 pressed，就把这次零样本视为毛刺。
                    ++g_release_glitch_suppressed_count;
                    if (g_release_glitch_suppressed_count == 1U ||
                        (g_release_glitch_suppressed_count % 32U) == 0U) {
                        ESP_LOGI(TAG,
                            "触摸 RELEASE 毛刺已抑制：count=%lu candidate_samples=%u",
                            static_cast<unsigned long>(g_release_glitch_suppressed_count),
                            static_cast<unsigned>(release_candidate_samples));
                    }
                }
                release_candidate_samples = 0U;

                const bool edge = !stable_pressed;
                stable_pressed = true;
                touch_publish_snapshot(point, now_ms, edge);
            } else if (stable_pressed) {
                if (release_candidate_samples < TOUCH_RELEASE_DEBOUNCE_SAMPLES) {
                    ++release_candidate_samples;
                }

                if (release_candidate_samples >= TOUCH_RELEASE_DEBOUNCE_SAMPLES) {
                    // RELEASE 延迟到连续 3 个零触点样本后确认。8ms 采样下只增加约 16ms
                    // 的抬手确认延迟，却能阻断 0/1 指抖动产生的 DOWN/UP 风暴。
                    stable_pressed = false;
                    release_candidate_samples = 0U;
                    touch_publish_snapshot(point, now_ms, true);
                }
                // 未达到确认阈值时保持上一份 pressed 快照，不发布伪 RELEASE。
            } else {
                release_candidate_samples = 0U;
            }
        } else {
            // 短暂 I2C 错误时保留最近稳定状态，不推进 RELEASE debounce，
            // 避免总线忙导致一次假抬手。
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
    g_edge_coalesce_count = 0U;
    g_edge_coalesced_events = 0U;
    g_release_glitch_suppressed_count = 0U;

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
#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(TAG,
        "Touch Fast Path：core=%ld priority=%u poll=%ums/%utick edge_queue=%u release_debounce=%u samples；MOVE仅latest snapshot；队列满时合并旧边沿并优先保留最新DOWN/UP",
        static_cast<long>(TOUCH_INPUT_TASK_CORE),
        static_cast<unsigned>(TOUCH_INPUT_TASK_PRIORITY),
        static_cast<unsigned>(TOUCH_SAMPLE_PERIOD_MS),
        static_cast<unsigned>(TOUCH_SAMPLE_PERIOD_TICKS),
        static_cast<unsigned>(TOUCH_EDGE_QUEUE_LENGTH),
        static_cast<unsigned>(TOUCH_RELEASE_DEBOUNCE_SAMPLES));
#endif
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
