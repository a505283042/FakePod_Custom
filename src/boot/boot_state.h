#pragma once


// 系统启动状态
enum class BootState
{
WaitStart,          // 等待启动,
    CheckPsram,     // 检查PSRAM,
    InitI2C,        // 初始化 I2C 总线
    InitTouch,      // 初始化触摸芯片
    InitIMU,        // 初始化 IMU 芯片
    InitSDCard,     // 初始化 TF 卡
    Ready,          // 系统准备就绪
    Error           // 启动失败
};


// 初始化启动状态机
void boot_state_init();


// 每次主循环调用一次
void boot_state_update();


// 判断启动流程是否已经完成
bool boot_state_is_ready();


// 判断启动是否失败
bool boot_state_has_error();


// 获取当前启动状态
BootState boot_state_get();