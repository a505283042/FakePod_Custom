#include "music_app_adapter.h"

#include "app_manager.h"
#include "esp_log.h"
#include "ui/screens/player_home.h"

static const char *TAG = "MusicAPP";

namespace {

esp_err_t music_create()
{
    // APP.1 尚未把 Music 的 Legacy LVGL 对象改造成可 destroy/recreate root。
    // 正常 Foreground/Background 往返不会经过 create；若状态机异常尝试重建，明确拒绝。
    ESP_LOGW(TAG, "Music create尚未开放：Legacy UI仍为常驻对象");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t music_enter()
{
    return player_home_app_enter_foreground();
}

esp_err_t music_leave(AppRunState next_state)
{
    if (next_state != AppRunState::Background) {
        ESP_LOGW(TAG, "Music暂不允许进入Stopped：next=%s", app_manager_state_name(next_state));
        return ESP_ERR_NOT_SUPPORTED;
    }
    return player_home_app_leave_background();
}

void music_destroy()
{
    // leave(Stopped) 在 APP.1 必定先拒绝，因此正常状态机不会进入这里。
    ESP_LOGW(TAG, "忽略Music destroy：APP.1仅支持后台挂起，不销毁Legacy UI");
}

} // namespace

esp_err_t music_app_adapter_bind()
{
    AppLifecycle lifecycle = {};
    lifecycle.create = music_create;
    lifecycle.enter = music_enter;
    lifecycle.leave = music_leave;
    lifecycle.destroy = music_destroy;

    const esp_err_t ret = app_manager_bind_adopted_lifecycle(AppId::Music, lifecycle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Music生命周期已接管：Foreground<->Background可用，Stopped保持保护");
    }
    return ret;
}
