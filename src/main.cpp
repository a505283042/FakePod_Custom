#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "boot_state.h"
#include "system_loop.h"

namespace
{
static const char *TAG = "系统主循环";
static constexpr uint32_t kRuntimeTaskStackBytes = 6144U;
static constexpr UBaseType_t kRuntimeTaskPriority = 1U;
static constexpr BaseType_t kRuntimeTaskCore = 0;

static void runtime_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "运行期任务接管：stack=%uB priority=%u core=%d hwm=%uB",
        static_cast<unsigned>(kRuntimeTaskStackBytes),
        static_cast<unsigned>(kRuntimeTaskPriority),
        static_cast<int>(kRuntimeTaskCore),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    while (true) {
        system_loop_update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
} // namespace

extern "C" void app_main(void)
{
    // 初始化启动状态机
    boot_state_init();

    // Boot Orchestrator 只运行到第一个终态；READY 后不再重复推进启动状态机。
    BootRunResult boot_result = BootRunResult::Running;
    while (boot_result == BootRunResult::Running) {
        boot_result = boot_run();
        if (boot_result == BootRunResult::Running) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (boot_result == BootRunResult::Fatal) {
        // 致命启动故障已经由 Boot 层记录并尽可能显示错误页。
        // 不发布 READY、不运行任何业务循环，也不重复尝试初始化硬件。
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (boot_result == BootRunResult::Service) {
        // TF卡 USB MSC 为独占维护模式：TinyUSB 自有任务继续服务电脑，
        // FakePod 正常 system_loop 永不启动，因此应用侧不会同时访问 TF 卡。
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // NORMAL / DEGRADED 的运行期从 IDF main task 迁到显式 6KB Core0/P1 任务。
    // 这不是新增一条并行业务线：创建成功后 app_main 立即返回，IDF 会释放原 main task。
    // 目的仅是给已经实测发生过 stack overflow 的运行期调用链留下可测量余量。
    const BaseType_t created = xTaskCreatePinnedToCore(
        runtime_task,
        "SystemLoop",
        kRuntimeTaskStackBytes,
        nullptr,
        kRuntimeTaskPriority,
        nullptr,
        kRuntimeTaskCore);
    if (created == pdPASS) {
        return;
    }

    // 极端内存不足时保留旧 main task 运行路径，避免因诊断修复本身导致无法进入 READY。
    ESP_LOGE(TAG, "运行期任务创建失败，回退IDF main task");
    while (true) {
        system_loop_update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}