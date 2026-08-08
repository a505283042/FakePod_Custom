#include "player_control.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "audio_service.h"
#include "player_state.h"

static const char *TAG = "播放器控制";
static SemaphoreHandle_t g_transport_lock = nullptr;

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
    ESP_LOGI(TAG, "Player transport 串行锁已就绪");
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

void player_control_update()
{
    // UI 与 loopTask 可能运行在不同任务；EOF 自动续播与手动切歌必须串行修改 Playlist Context。
    if (!player_control_lock(0)) {
        return;
    }
    player_transport_update();
    player_control_unlock();
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
    const bool audio_is_selected_track =
        snapshot.track_index != UINT32_MAX && snapshot.track_index == selected_track;

    bool ok = false;
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

PlayerLoopMode player_control_get_loop_mode()
{
    return player_transport_get_loop_mode();
}

PlayerLoopMode player_control_cycle_loop_mode()
{
    return player_transport_cycle_loop_mode();
}

bool player_control_set_loop_mode(PlayerLoopMode mode)
{
    return player_transport_set_loop_mode(mode);
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
