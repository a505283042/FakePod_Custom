#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "boot_state.h"
#include "system_loop.h"

extern "C" void app_main(void)
{
    // 初始化启动状态机
    boot_state_init();

    // 主循环只负责顶层调度
    while (true) {

        // Boot Orchestrator 是唯一启动推进入口；业务循环只在 READY 后工作。
        (void)boot_run();

        system_loop_update();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}