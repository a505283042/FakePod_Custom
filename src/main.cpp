#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "boot_state.h"
#include "system_loop.h"

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

    // NORMAL / DEGRADED 都共享同一运行期入口；后台服务仍由 READY 闸门统一启动。
    while (true) {
        system_loop_update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}