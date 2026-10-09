#include "player_control.h"

#include <atomic>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "app_diag_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "audio_service.h"
#include "device_settings.h"
#include "player_state.h"

static const char *TAG = "播放器控制";
static SemaphoreHandle_t g_transport_lock = nullptr;

// 自然 EOF 只在 SystemLoop 观察边沿；真正的 Playlist/Transport 续播统一投递到
// LVGL/Player 上下文。状态 0=空闲，1=等待投递，2=已进入 LVGL async 队列。
// 单槽足够：没有执行本次 EOF 前，不可能自然产生下一首的第二个 EOF。
static std::atomic<uint8_t> g_auto_next_dispatch_state{0U};
static AudioStateSnapshot g_auto_next_snapshot = {};
static constexpr uint8_t AUTO_NEXT_IDLE = 0U;
static constexpr uint8_t AUTO_NEXT_PENDING = 1U;
static constexpr uint8_t AUTO_NEXT_QUEUED = 2U;

static PlayerFolderScope player_control_scope_from_setting(DeviceMusicListScope scope)
{
    switch (scope) {
        case DeviceMusicListScope::All: return PlayerFolderScope::All;
        case DeviceMusicListScope::Level1: return PlayerFolderScope::Level1;
        case DeviceMusicListScope::Level2: return PlayerFolderScope::Level2;
    }
    return PlayerFolderScope::All;
}

static bool player_control_current_folder_path(
    PlayerFolderScope scope, char *buffer, size_t buffer_size)
{
    if ((scope != PlayerFolderScope::Level1 && scope != PlayerFolderScope::Level2) ||
        buffer == nullptr || buffer_size == 0U) {
        return false;
    }
    buffer[0] = '\0';
    const char *track_path = player_state_get_path();
    static constexpr const char *kMusicPrefix = "/sdcard/MUSIC/";
    const size_t root_length = strlen(kMusicPrefix);
    if (track_path == nullptr || strncasecmp(track_path, kMusicPrefix, root_length) != 0) {
        return false;
    }
    const char *relative = track_path + root_length;
    const char *first = strchr(relative, '/');
    if (first == nullptr) return false;
    const char *end = first;
    if (scope == PlayerFolderScope::Level2) {
        const char *second = strchr(first + 1U, '/');
        if (second == nullptr) return false;
        end = second;
    }
    const size_t length = static_cast<size_t>(end - track_path) + 1U;
    if (length + 1U > buffer_size) return false;
    memcpy(buffer, track_path, length);
    buffer[length] = '\0';
    return player_playlist_folder_selection_available(scope, buffer, nullptr);
}

static bool player_control_first_folder_path(
    PlayerFolderScope scope, char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U) return false;
    buffer[0] = '\0';
    if (scope == PlayerFolderScope::Level1) {
        return player_playlist_copy_folder_option_at(
            PlayerFolderScope::Level1, nullptr, 0U, buffer, buffer_size, nullptr);
    }
    if (scope != PlayerFolderScope::Level2) return false;
    const size_t parent_count = player_playlist_get_folder_option_count(PlayerFolderScope::Level1, nullptr);
    for (size_t parent = 0U; parent < parent_count; ++parent) {
        char parent_path[PLAYER_FOLDER_PATH_MAX] = {};
        if (!player_playlist_copy_folder_option_at(
                PlayerFolderScope::Level1, nullptr, parent,
                parent_path, sizeof(parent_path), nullptr)) {
            continue;
        }
        if (player_playlist_copy_folder_option_at(
                PlayerFolderScope::Level2, parent_path, 0U,
                buffer, buffer_size, nullptr)) {
            return true;
        }
    }
    return false;
}

static bool player_control_resolve_folder_path(
    PlayerFolderScope scope, char *buffer, size_t buffer_size)
{
    return player_control_current_folder_path(scope, buffer, buffer_size) ||
        player_control_first_folder_path(scope, buffer, buffer_size);
}

esp_err_t player_control_init()
{
    if (g_transport_lock != nullptr) {
        return ESP_OK;
    }
    g_transport_lock = xSemaphoreCreateMutex();
    if (g_transport_lock == nullptr) {
        ESP_LOGE(TAG, "创建 Player transport 串行锁失败");
        return ESP_ERR_NO_MEM;
    }

    // DeviceSettings 在 Boot NVS 阶段已经加载；PlayerState 此时也已建立。
    // 目录播放范围只影响后续 transport，不会在启动时触发播放或重新打开音频。
    DeviceSettingsSnapshot settings = {};
    DeviceMusicListSelection selection = {};
    if (device_settings_get_snapshot(&settings)) {
        (void)device_settings_get_music_list_selection_for_source(
            DeviceMusicLibrarySource::Local, &selection);
        bool level1_ready = selection.level1_path[0] != '\0' &&
            player_playlist_set_folder_selection(PlayerFolderScope::Level1, selection.level1_path);
        bool level2_ready = selection.level2_path[0] != '\0' &&
            player_playlist_set_folder_selection(PlayerFolderScope::Level2, selection.level2_path);
        if (selection.level1_path[0] != '\0' && !level1_ready) {
            ESP_LOGW(TAG, "已保存一级目录当前不可用：%s", selection.level1_path);
        }
        if (selection.level2_path[0] != '\0' && !level2_ready) {
            ESP_LOGW(TAG, "已保存二级目录当前不可用：%s", selection.level2_path);
        }

        PlayerFolderScope scope = player_control_scope_from_setting(selection.scope);
        bool repaired_selection = false;
        if (scope == PlayerFolderScope::Level1 && !level1_ready) {
            char resolved[PLAYER_FOLDER_PATH_MAX] = {};
            if (player_control_resolve_folder_path(scope, resolved, sizeof(resolved)) &&
                player_playlist_set_folder_selection(scope, resolved)) {
                snprintf(selection.level1_path, sizeof(selection.level1_path), "%s", resolved);
                level1_ready = true;
                repaired_selection = true;
                ESP_LOGI(TAG, "一级列表自动建立初始目录：%s", resolved);
            }
        } else if (scope == PlayerFolderScope::Level2 && !level2_ready) {
            char resolved[PLAYER_FOLDER_PATH_MAX] = {};
            if (player_control_resolve_folder_path(scope, resolved, sizeof(resolved)) &&
                player_playlist_set_folder_selection(scope, resolved)) {
                snprintf(selection.level2_path, sizeof(selection.level2_path), "%s", resolved);
                level2_ready = true;
                repaired_selection = true;
                ESP_LOGI(TAG, "二级列表自动建立初始目录：%s", resolved);
            }
        }

        if (repaired_selection) {
            const esp_err_t save_ret = device_settings_set_music_list_selection_for_source(
                DeviceMusicLibrarySource::Local, selection.scope,
                selection.level1_path, selection.level2_path);
            if (save_ret != ESP_OK) {
                ESP_LOGW(TAG, "保存自动建立的播放目录失败：%s", esp_err_to_name(save_ret));
            }
        }

        if ((scope == PlayerFolderScope::Level1 && !level1_ready) ||
            (scope == PlayerFolderScope::Level2 && !level2_ready)) {
            ESP_LOGW(TAG, "本地播放列表目录尚不可用，持久状态回退本地总列表：setting=%s",
                device_settings_music_list_scope_name(selection.scope));
            scope = PlayerFolderScope::All;
            const esp_err_t save_ret = device_settings_set_music_list_selection_for_source(
                DeviceMusicLibrarySource::Local, DeviceMusicListScope::All,
                selection.level1_path, selection.level2_path);
            if (save_ret != ESP_OK) {
                ESP_LOGW(TAG, "保存本地总列表回退失败：%s", esp_err_to_name(save_ret));
            }
        }
        if (!player_playlist_set_folder_scope(scope)) {
            ESP_LOGW(TAG, "应用音乐列表范围失败：%s",
                device_settings_music_list_scope_name(selection.scope));
        }
    }

#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(TAG, "Player transport 串行锁已就绪");
#endif
    return ESP_OK;
}

static bool player_control_lock(TickType_t wait_ticks)
{
    return g_transport_lock != nullptr && xSemaphoreTake(g_transport_lock, wait_ticks) == pdTRUE;
}

static void player_control_unlock()
{
    if (g_transport_lock != nullptr) {
        xSemaphoreGive(g_transport_lock);
    }
}

static void player_control_auto_next_async(void *)
{
    if (g_auto_next_dispatch_state.load(std::memory_order_acquire) != AUTO_NEXT_QUEUED) {
        return;
    }
    const AudioStateSnapshot snapshot = g_auto_next_snapshot;
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        // 不丢 EOF：锁暂时繁忙时退回 Pending，由下一拍 SystemLoop 再次投递。
        g_auto_next_dispatch_state.store(AUTO_NEXT_PENDING, std::memory_order_release);
        ESP_LOGW(TAG, "AUTO_NEXT_DISPATCH: LVGL取得Player锁超时，等待下一拍重投");
        return;
    }

    player_transport_handle_finished_event(snapshot);
    player_control_unlock();
    g_auto_next_dispatch_state.store(AUTO_NEXT_IDLE, std::memory_order_release);
}

static void player_control_try_dispatch_auto_next()
{
    uint8_t expected = AUTO_NEXT_PENDING;
    if (!g_auto_next_dispatch_state.compare_exchange_strong(
            expected, AUTO_NEXT_QUEUED,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }

    if (lv_async_call(player_control_auto_next_async, nullptr) != LV_RESULT_OK) {
        // LVGL 队列偶发分配失败也不能吞掉自然 EOF；保持 Pending，下一拍再试。
        g_auto_next_dispatch_state.store(AUTO_NEXT_PENDING, std::memory_order_release);
        ESP_LOGW(TAG, "AUTO_NEXT_DISPATCH: LVGL投递失败，等待下一拍重投");
    }
}

void player_control_update()
{
    // 先补投上一拍未能进入 LVGL 队列的自然 EOF；正常情况下这里为空操作。
    player_control_try_dispatch_auto_next();

    // SystemLoop 只做轻量 Snapshot 边沿检测。Track Error 仍沿用原同步处理；
    // Finished 不再在这条 6KB SystemLoop 调用栈中执行深层 Playlist/URL/Audio request。
    if (!player_control_lock(0)) {
        return;
    }
    AudioStateSnapshot finished = {};
    const bool has_finished = player_transport_update(&finished);
    player_control_unlock();

    if (!has_finished) {
        return;
    }

    if (g_auto_next_dispatch_state.load(std::memory_order_acquire) != AUTO_NEXT_IDLE) {
        // 理论上不会发生：上一首未真正续播前不可能自然产生第二个 EOF。
        ESP_LOGW(TAG,
            "AUTO_NEXT_DISPATCH: 已有待处理EOF，忽略重复边沿 track=%lu playback_rev=%lu",
            static_cast<unsigned long>(finished.track_index),
            static_cast<unsigned long>(finished.playback_revision));
        return;
    }

    g_auto_next_snapshot = finished;
    g_auto_next_dispatch_state.store(AUTO_NEXT_PENDING, std::memory_order_release);
    ESP_LOGI(TAG,
        "AUTO_NEXT_DISPATCH: 自然EOF已投递Player/LVGL track=%lu playback_rev=%lu",
        static_cast<unsigned long>(finished.track_index),
        static_cast<unsigned long>(finished.playback_revision));
    player_control_try_dispatch_auto_next();
}

bool player_control_play_current()
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_transport_play_current("用户选择播放");
    player_control_unlock();
    return ok;
}

bool player_control_toggle_play_pause()
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }

    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        ESP_LOGW(TAG, "AudioTask 尚未就绪");
        player_control_unlock();
        return false;
    }


    const size_t selected_track = player_state_get_index();
    const AudioPlaybackSource expected_source = player_state_get_source() == PlayerMediaSource::Nas
        ? AudioPlaybackSource::NasHttp : AudioPlaybackSource::Local;
    const bool audio_is_selected_track =
        snapshot.source == expected_source &&
        snapshot.track_index != UINT32_MAX && snapshot.track_index == selected_track;

    bool ok = false;
    if (audio_is_selected_track && snapshot.state == AudioPlaybackState::Seeking) {
        ESP_LOGW(TAG, "Seek进行中，忽略播放/暂停操作");
        player_control_unlock();
        return false;
    }
    if (audio_is_selected_track && snapshot.state == AudioPlaybackState::Playing) {
        ok = audio_service_pause(false);
    } else if (audio_is_selected_track && snapshot.state == AudioPlaybackState::Paused) {
        ok = audio_service_resume(false);
    } else {
        // UI 可能已经切换了 Playlist Context，但旧 Track 仍在播放。
        // 此时“播放”必须启动新选中 Track，而不是暂停旧 Track。
        ok = player_transport_play_current("播放/暂停键起播");
    }

    player_control_unlock();
    return ok;
}

bool player_control_previous()
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_transport_previous();
    player_control_unlock();
    return ok;
}

bool player_control_next()
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_transport_next();
    player_control_unlock();
    return ok;
}

bool player_control_peek_next_track(uint32_t *out_track_index)
{
    if (out_track_index == nullptr) return false;
    *out_track_index = UINT32_MAX;
    // 预热只是后台机会任务，绝不能为了拿下一首提示阻塞 LVGL。
    if (!player_control_lock(0)) return false;
    (void)player_transport_peek_next_track(out_track_index);
    player_control_unlock();
    return true;
}

PlayerLoopMode player_control_get_loop_mode()
{
    return player_transport_get_loop_mode();
}

PlayerLoopMode player_control_cycle_loop_mode()
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return player_transport_get_loop_mode();
    }
    const PlayerLoopMode next = player_transport_cycle_loop_mode();
    player_control_unlock();
    return next;
}

bool player_control_set_loop_mode(PlayerLoopMode mode)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_transport_set_loop_mode(mode);
    player_control_unlock();
    return ok;
}

PlayerFolderScope player_control_get_folder_scope()
{
    return player_state_get_folder_scope();
}

bool player_control_set_folder_scope(PlayerFolderScope scope)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_state_set_folder_scope(scope);
    player_control_unlock();
    return ok;
}

bool player_control_set_folder_selection(PlayerFolderScope scope, const char *folder_path)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_state_set_folder_selection(scope, folder_path);
    player_control_unlock();
    return ok;
}

bool player_control_copy_folder_selection(
    PlayerFolderScope scope, char *buffer, size_t buffer_size)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_state_copy_folder_selection(scope, buffer, buffer_size);
    player_control_unlock();
    return ok;
}

bool player_control_set_volume(uint8_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }
    return audio_service_set_volume(percent, true);
}

bool player_control_volume_up(uint8_t step)
{
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        return false;
    }
    const uint16_t next = static_cast<uint16_t>(snapshot.volume_percent) + step;
    return player_control_set_volume(static_cast<uint8_t>(next > 100U ? 100U : next));
}

bool player_control_volume_down(uint8_t step)
{
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        return false;
    }
    const uint8_t next = snapshot.volume_percent > step
        ? static_cast<uint8_t>(snapshot.volume_percent - step)
        : 0U;
    return player_control_set_volume(next);
}

bool player_control_toggle_mute()
{
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        return false;
    }
    return audio_service_set_mute(!snapshot.user_muted, true);
}


bool player_control_seek_ms(uint64_t target_ms)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    AudioStateSnapshot audio = {};
    if (audio_service_get_snapshot(&audio) && audio.source == AudioPlaybackSource::NasHttp) {
        ESP_LOGW(TAG, "NAS顺序流当前不支持Seek：target=%llums",
            static_cast<unsigned long long>(target_ms));
        player_control_unlock();
        return false;
    }
    const bool ok = player_transport_seek_ms(target_ms);
    player_control_unlock();
    return ok;
}

static bool player_control_select_context(bool selected)
{
    if (!selected) {
        ESP_LOGW(TAG, "切换播放列表失败，保留原列表上下文");
        return false;
    }
    return true;
}

bool player_control_select_all_tracks(size_t position)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_control_select_context(player_state_select_all_tracks(position));
    player_control_unlock();
    return ok;
}

bool player_control_select_folder_queue_position(size_t position)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_control_select_context(
        player_state_select_folder_queue_position(position));
    player_control_unlock();
    return ok;
}

bool player_control_select_local_folder_queue_position(size_t position)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_control_select_context(
        player_state_select_local_folder_queue_position(position));
    player_control_unlock();
    return ok;
}

bool player_control_select_artist_group(size_t group_index, size_t position)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_control_select_context(player_state_select_artist_group(group_index, position));
    player_control_unlock();
    return ok;
}

bool player_control_select_album_group(size_t group_index, size_t position)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_control_select_context(player_state_select_album_group(group_index, position));
    player_control_unlock();
    return ok;
}

bool player_control_select_decade_group(size_t group_index, size_t position)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool ok = player_control_select_context(player_state_select_decade_group(group_index, position));
    player_control_unlock();
    return ok;
}


bool player_control_select_nas_all_tracks(size_t position)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) return false;
    const bool ok = player_control_select_context(player_state_select_nas_all_tracks(position));
    player_control_unlock();
    return ok;
}

bool player_control_select_nas_folder_queue_position(size_t position)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) return false;
    const bool ok = player_control_select_context(player_state_select_nas_folder_queue_position(position));
    player_control_unlock();
    return ok;
}

bool player_control_select_nas_artist_group(size_t group_index, size_t position)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) return false;
    const bool ok = player_control_select_context(player_state_select_nas_artist_group(group_index, position));
    player_control_unlock();
    return ok;
}

bool player_control_select_nas_album_group(size_t group_index, size_t position)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) return false;
    const bool ok = player_control_select_context(player_state_select_nas_album_group(group_index, position));
    player_control_unlock();
    return ok;
}

bool player_control_select_nas_decade_group(size_t group_index, size_t position)
{
    if (!player_control_lock(pdMS_TO_TICKS(100))) return false;
    const bool ok = player_control_select_context(player_state_select_nas_decade_group(group_index, position));
    player_control_unlock();
    return ok;
}
