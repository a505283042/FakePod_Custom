#pragma once


// ============================================================
// 系统启动状态
// ============================================================

enum class BootState
{
    WaitStart,

    CheckPsram,

    InitI2C,

    InitTouch,

    InitIMU,

    InitSDCard,

    ScanMediaLibrary,

    InitDisplay,

    InitUI,

    Ready,

    Error
};


// 初始化启动状态机
void boot_state_init();


// 更新启动状态机
void boot_state_update();


// 判断启动流程是否完成
bool boot_state_is_ready();


// 判断启动是否失败
bool boot_state_has_error();


// 获取当前启动状态
BootState boot_state_get();