#pragma once

// ============================================================
// 系统启动状态
// ============================================================
enum class BootState
{
    CheckPsram,
    InitI2C,
    InitTouch,
    InitIMU,
    InitAudioService,
    InitSDCard,
    ScanMediaLibrary,
    InitDisplay,
    InitUIBootstrap,
    InitUI,
    Ready,
    Error
};

// 启动编排器的顶层推进结果。
// 本阶段先建立统一入口；后续再扩展降级/可选模块结果。
enum class BootRunResult
{
    Running,
    Ready,
    Fatal
};

// 启动编排器顶层入口：每次调用推进一个既有启动阶段，并返回统一结果。
BootRunResult boot_run();

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
