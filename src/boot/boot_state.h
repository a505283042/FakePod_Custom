#pragma once

#include <stdint.h>

#include "esp_err.h"

// ============================================================
// 系统启动状态
// ============================================================
enum class BootState
{
    CheckPsram,
    InitI2C,
    InitDisplay,
    InitUIBootstrap,
    InitTouch,
    InitIMU,
    InitAudioService,
    InitSDCard,
    ScanMediaLibrary,
    InitPlayer,
    InitUI,
    Ready,
    Error
};

// 可被 Boot Snapshot 稳定记录的启动问题。bit 值长期只增不复用。
enum class BootIssue : uint32_t
{
    None = 0U,
    TouchUnavailable = 1U << 0,
    ImuUnavailable = 1U << 1,
    StorageUnavailable = 1U << 2,
    LibraryUnavailable = 1U << 3,
    PlayerStateUnavailable = 1U << 4,
    PlayerControlUnavailable = 1U << 5,
    PsramUnavailable = 1U << 16,
    I2cUnavailable = 1U << 17,
    DisplayUnavailable = 1U << 18,
    UiBootstrapUnavailable = 1U << 19,
    AudioUnavailable = 1U << 20,
    UiUnavailable = 1U << 21
};

// 启动编排器的顶层推进结果。
enum class BootRunResult
{
    Running,
    Ready,
    ReadyDegraded,
    Fatal
};

struct BootStatusSnapshot
{
    BootState state;
    uint32_t degraded_issues;
    uint32_t optional_issues;
    BootIssue fatal_issue;
    esp_err_t fatal_error;
};

// 启动编排器顶层入口：每次调用推进一个启动阶段，并返回统一结果。
BootRunResult boot_run();

// 初始化启动状态机。
void boot_state_init();

// 判断启动流程是否完成。system_loop 只依赖这个稳定边界。
bool boot_state_is_ready();

// 获取轻量启动状态快照，供后续设置页/诊断页展示。
bool boot_state_get_status(BootStatusSnapshot *out_status);
