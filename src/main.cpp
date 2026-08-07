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

        boot_state_update();

        system_loop_update();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}