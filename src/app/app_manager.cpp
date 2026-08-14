#include "app_manager.h"

#include <stddef.h>

#include "esp_log.h"

static const char *TAG = "APP管理";

namespace {

struct AppSlot {
    AppDescriptor descriptor = {};
    AppRunState state = AppRunState::Stopped;
    bool registered = false;
    bool created = false;
    bool adopted_legacy = false;
};

static AppSlot g_slots[static_cast<size_t>(AppId::Count)] = {};
static bool g_ready = false;
static AppId g_foreground = AppId::Music;
static AppId g_launcher_target = AppId::Music;

static constexpr const char *kBuiltinNames[static_cast<size_t>(AppId::Count)] = {
    "音乐",
    "NSF播放",
    "拾音频谱",
    "MJPG播放",
    "图片播放",
    "电子书",
    "设置",
};

static bool app_id_valid(AppId id)
{
    return static_cast<size_t>(id) < static_cast<size_t>(AppId::Count);
}

static AppSlot *slot_for(AppId id)
{
    return app_id_valid(id) ? &g_slots[static_cast<size_t>(id)] : nullptr;
}

static const AppSlot *slot_for_const(AppId id)
{
    return app_id_valid(id) ? &g_slots[static_cast<size_t>(id)] : nullptr;
}

static esp_err_t ensure_created(AppSlot *slot)
{
    if (slot == nullptr || !slot->registered) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (slot->created) {
        return ESP_OK;
    }

    const esp_err_t ret = slot->descriptor.lifecycle.create != nullptr
        ? slot->descriptor.lifecycle.create()
        : ESP_OK;
    if (ret == ESP_OK) {
        slot->created = true;
    }
    return ret;
}

static esp_err_t enter_slot(AppSlot *slot)
{
    if (slot == nullptr || !slot->registered || !slot->created) {
        return ESP_ERR_INVALID_STATE;
    }
    return slot->descriptor.lifecycle.enter != nullptr
        ? slot->descriptor.lifecycle.enter()
        : ESP_OK;
}

static esp_err_t leave_slot(AppSlot *slot, AppRunState next_state)
{
    if (slot == nullptr || !slot->registered || !slot->created) {
        return ESP_ERR_INVALID_STATE;
    }
    return slot->descriptor.lifecycle.leave != nullptr
        ? slot->descriptor.lifecycle.leave(next_state)
        : ESP_OK;
}

static void destroy_slot(AppSlot *slot)
{
    if (slot == nullptr || !slot->created) {
        return;
    }
    if (slot->descriptor.lifecycle.destroy != nullptr) {
        slot->descriptor.lifecycle.destroy();
    }
    slot->created = false;
    slot->state = AppRunState::Stopped;
}

} // namespace

esp_err_t app_manager_init()
{
    if (g_ready) {
        return ESP_OK;
    }

    for (size_t i = 0; i < static_cast<size_t>(AppId::Count); ++i) {
        g_slots[i] = {};
        g_slots[i].descriptor.id = static_cast<AppId>(i);
        g_slots[i].descriptor.name = kBuiltinNames[i];
    }

    // Core Platform V1 之前的 Music 已由 Boot/UI/Audio 正常创建；APP.0 只“接管状态”，
    // 不重复 create/enter，避免破坏已经验证的 AudioTask / Player / LVGL 启动链。
    AppSlot &music = g_slots[static_cast<size_t>(AppId::Music)];
    music.registered = true;
    music.created = true;
    music.state = AppRunState::Foreground;
    music.descriptor.supports_background = true;
    music.adopted_legacy = true;

    g_foreground = AppId::Music;
    g_launcher_target = AppId::Music;
    g_ready = true;

    ESP_LOGI(TAG, "App Manager就绪：foreground=%s slots=%u；Music采用Legacy Adopt模式",
        app_manager_name(g_foreground),
        static_cast<unsigned>(AppId::Count));
    return ESP_OK;
}

bool app_manager_is_ready()
{
    return g_ready;
}

esp_err_t app_manager_register(const AppDescriptor &descriptor)
{
    if (!g_ready || !app_id_valid(descriptor.id) || descriptor.id == AppId::Music) {
        return ESP_ERR_INVALID_STATE;
    }

    AppSlot *slot = slot_for(descriptor.id);
    if (slot == nullptr || slot->state != AppRunState::Stopped || slot->created) {
        return ESP_ERR_INVALID_STATE;
    }

    slot->descriptor = descriptor;
    if (slot->descriptor.name == nullptr || slot->descriptor.name[0] == '\0') {
        slot->descriptor.name = kBuiltinNames[static_cast<size_t>(descriptor.id)];
    }
    slot->registered = true;

    ESP_LOGI(TAG, "APP注册：id=%u name=%s background=%s",
        static_cast<unsigned>(descriptor.id),
        slot->descriptor.name,
        slot->descriptor.supports_background ? "YES" : "NO");
    return ESP_OK;
}

esp_err_t app_manager_bind_adopted_lifecycle(AppId id, const AppLifecycle &lifecycle)
{
    if (!g_ready || !app_id_valid(id)) {
        return ESP_ERR_INVALID_STATE;
    }
    AppSlot *slot = slot_for(id);
    if (slot == nullptr || !slot->registered || !slot->adopted_legacy ||
        !slot->created || slot->state == AppRunState::Stopped) {
        return ESP_ERR_INVALID_STATE;
    }

    slot->descriptor.lifecycle = lifecycle;
    slot->adopted_legacy = false;
    ESP_LOGI(TAG, "Legacy APP生命周期已接管：%s state=%s",
        app_manager_name(id), app_manager_state_name(slot->state));
    return ESP_OK;
}

esp_err_t app_manager_request_foreground(AppId id, AppTransitionMode mode)
{
    if (!g_ready || !app_id_valid(id)) {
        return ESP_ERR_INVALID_STATE;
    }

    AppSlot *target = slot_for(id);
    if (target == nullptr || !target->registered) {
        ESP_LOGW(TAG, "APP尚未注册：id=%u name=%s",
            static_cast<unsigned>(id), app_manager_name(id));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (id == g_foreground && target->state == AppRunState::Foreground) {
        return ESP_OK;
    }

    AppSlot *current = slot_for(g_foreground);
    const AppId previous_id = g_foreground;
    if (current != nullptr && current->adopted_legacy && previous_id != id) {
        ESP_LOGW(TAG, "拒绝离开Legacy APP：%s 尚未绑定生命周期适配器", app_manager_name(previous_id));
        return ESP_ERR_NOT_SUPPORTED;
    }

    // 先把目标 create 完成，再动当前前台；create 失败时现有 APP 完全不受影响。
    esp_err_t ret = ensure_created(target);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "APP create失败：%s ret=%s", app_manager_name(id), esp_err_to_name(ret));
        return ret;
    }
    AppRunState previous_next = AppRunState::Stopped;
    if (current != nullptr && current->registered && current->created &&
        current->state == AppRunState::Foreground) {
        const bool keep_background = mode == AppTransitionMode::PreserveBackground &&
            current->descriptor.supports_background;
        previous_next = keep_background ? AppRunState::Background : AppRunState::Stopped;
        ret = leave_slot(current, previous_next);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "APP leave失败：%s ret=%s", app_manager_name(previous_id), esp_err_to_name(ret));
            if (target->state == AppRunState::Stopped) {
                destroy_slot(target);
            }
            return ret;
        }
        current->state = previous_next;
        if (previous_next == AppRunState::Stopped) {
            destroy_slot(current);
        }
    }

    // 从旧前台 leave 成功开始，到目标 enter / rollback 成功之前，不对外谎报任何前台 APP。
    g_foreground = AppId::None;
    ret = enter_slot(target);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "APP enter失败：%s ret=%s；尝试恢复%s",
            app_manager_name(id), esp_err_to_name(ret), app_manager_name(previous_id));
        destroy_slot(target);

        AppSlot *previous = slot_for(previous_id);
        if (previous != nullptr && previous->registered) {
            if (ensure_created(previous) == ESP_OK && enter_slot(previous) == ESP_OK) {
                previous->state = AppRunState::Foreground;
                g_foreground = previous_id;
            } else {
                ESP_LOGE(TAG, "APP rollback失败：系统暂时无Foreground APP");
            }
        }
        return ret;
    }

    target->state = AppRunState::Foreground;
    g_foreground = id;
    ESP_LOGI(TAG, "前台切换：%s -> %s mode=%s",
        app_manager_name(previous_id),
        app_manager_name(id),
        mode == AppTransitionMode::PreserveBackground ? "PreserveBackground" : "Exclusive");
    return ESP_OK;
}

esp_err_t app_manager_stop(AppId id)
{
    if (!g_ready || !app_id_valid(id)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (id == g_foreground) {
        return ESP_ERR_INVALID_STATE;
    }

    AppSlot *slot = slot_for(id);
    if (slot != nullptr && slot->adopted_legacy) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (slot == nullptr || !slot->registered || slot->state == AppRunState::Stopped) {
        return ESP_OK;
    }

    if (slot->created) {
        const esp_err_t ret = leave_slot(slot, AppRunState::Stopped);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    destroy_slot(slot);
    return ESP_OK;
}

void app_manager_set_launcher_target(AppId id)
{
    if (!g_ready || !app_id_valid(id)) {
        return;
    }
    g_launcher_target = id;
}

AppId app_manager_launcher_target()
{
    return g_launcher_target;
}

AppId app_manager_foreground()
{
    return g_foreground;
}

AppRunState app_manager_state(AppId id)
{
    const AppSlot *slot = slot_for_const(id);
    return slot != nullptr ? slot->state : AppRunState::Stopped;
}

bool app_manager_is_registered(AppId id)
{
    const AppSlot *slot = slot_for_const(id);
    return slot != nullptr && slot->registered;
}

bool app_manager_supports_background(AppId id)
{
    const AppSlot *slot = slot_for_const(id);
    return slot != nullptr && slot->registered && slot->descriptor.supports_background;
}

const char *app_manager_name(AppId id)
{
    if (id == AppId::None) {
        return "None";
    }
    if (!app_id_valid(id)) {
        return "Unknown";
    }
    const AppSlot *slot = slot_for_const(id);
    if (slot != nullptr && slot->descriptor.name != nullptr && slot->descriptor.name[0] != '\0') {
        return slot->descriptor.name;
    }
    return kBuiltinNames[static_cast<size_t>(id)];
}

const char *app_manager_state_name(AppRunState state)
{
    switch (state) {
        case AppRunState::Stopped: return "Stopped";
        case AppRunState::Foreground: return "Foreground";
        case AppRunState::Background: return "Background";
        default: return "Unknown";
    }
}
