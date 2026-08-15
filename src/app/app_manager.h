#pragma once

#include <stdint.h>
#include "esp_err.h"

// APP.0：FakePod 多 APP 生命周期的唯一公共编排入口。
// App Manager 只管理 APP 的生命周期与前后台状态，不直接拥有 AudioTask、Display、Storage 等硬件资源。
enum class AppId : uint8_t {
    Music = 0,
    Nsf,
    MicSpectrum,
    Video,
    Picture,
    Ebook,
    Settings,
    Count,
    None = 0xFF,
};

enum class AppRunState : uint8_t {
    Stopped = 0,
    Foreground,
    Background,
};

enum class AppTransitionMode : uint8_t {
    // 当前前台 APP 必须离开并停止；适用于 FC、设置等独占前台场景。
    Exclusive = 0,
    // 当前前台 APP 若声明支持后台，则转入 Background；主要用于“音乐后台 + Reader 前台”。
    PreserveBackground,
};

struct AppLifecycle {
    // create/destroy 负责 APP 私有资源的完整建立/释放；enter/leave 只负责前后台切换。
    esp_err_t (*create)() = nullptr;
    esp_err_t (*enter)() = nullptr;
    esp_err_t (*leave)(AppRunState next_state) = nullptr;
    void (*destroy)() = nullptr;
};

struct AppDescriptor {
    AppId id = AppId::Music;
    const char *name = nullptr;
    bool supports_background = false;
    AppLifecycle lifecycle = {};
};

// 初始化固定 7 槽注册表，并把现有播放器主页作为已存在的 Music Foreground 接管。
// 不创建任务、不分配堆内存，也不调用 Music 的旧 UI/Audio 初始化路径。
esp_err_t app_manager_init();
bool app_manager_is_ready();

// 新 APP 在 create 前注册自己的生命周期。相同 AppId 仅允许在 Stopped 状态更新描述符。
esp_err_t app_manager_register(const AppDescriptor &descriptor);

// Core Platform V1 的 Music 是启动阶段已经创建好的 Legacy APP。
// 在第一次真实 APP 切换前必须绑定它的 leave/destroy/create/enter 适配器，之后 Manager 才允许离开 Music。
esp_err_t app_manager_bind_adopted_lifecycle(AppId id, const AppLifecycle &lifecycle);

// 请求切换前台 APP。目标未注册时返回 ESP_ERR_NOT_SUPPORTED；失败时尽量恢复原前台状态。
esp_err_t app_manager_request_foreground(AppId id, AppTransitionMode mode);

// 停止指定 APP 并释放其私有资源。当前前台 APP 不允许通过该接口直接停止。
esp_err_t app_manager_stop(AppId id);

// Launcher 只发布用户当前选中的目标；真正进入 APP 仍必须显式调用 request_foreground。
void app_manager_set_launcher_target(AppId id);
AppId app_manager_launcher_target();

AppId app_manager_foreground();
AppRunState app_manager_state(AppId id);
bool app_manager_is_registered(AppId id);
bool app_manager_supports_background(AppId id);
const char *app_manager_name(AppId id);
const char *app_manager_state_name(AppRunState state);
