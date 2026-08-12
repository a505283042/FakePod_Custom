#pragma once

// 发布系统核心依赖已经建立的唯一 READY 边界。
// 只允许 Boot Orchestrator 调用；后台服务不会在本函数内直接启动。
void system_ready_publish();

// READY 后由 system_loop 调用。第一次调用统一启动可选后台服务，之后为空操作。
void system_runtime_update();
