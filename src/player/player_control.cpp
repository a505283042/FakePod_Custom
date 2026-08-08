#include "player_control.h"

#include "esp_log.h"
#include "audio_service.h"
#include "player_state.h"

static const char *TAG = "播放器控制";

static bool player_control_stop_before_selection()
{
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        ESP_LOGW(TAG, "AudioTask 尚未就绪");
        return false;
    }

    if (
        snapshot.state == AudioPlaybackState::Ready ||
        snapshot.state == AudioPlaybackState::Stopped
    ) {
        return true;
    }

    if (!audio_service_stop(true)) {
        ESP_LOGE(TAG, "切歌前停止 AudioTask 失败，保留当前歌曲选择");
        return false;
    }
    return true;
}

bool player_control_toggle_play_pause()
{
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        ESP_LOGW(TAG, "AudioTask 尚未就绪");
        return false;
    }

    if (snapshot.state == AudioPlaybackState::Playing) {
        return audio_service_pause(false);
    }
    if (snapshot.state == AudioPlaybackState::Paused) {
        return audio_service_resume(false);
    }

    const char *path = player_state_get_path();
    if (path == nullptr) {
        ESP_LOGW(TAG, "当前没有可播放歌曲");
        return false;
    }

    const size_t track_index = player_state_get_index();
    MediaTechnicalInfo technical = {};
    const bool has_technical_info = media_library_get_technical_info(track_index, &technical);

    return audio_service_play_track(
        static_cast<uint32_t>(track_index),
        path,
        player_state_get_format(),
        has_technical_info ? &technical : nullptr,
        false
    );
}

bool player_control_previous()
{
    if (!player_control_stop_before_selection()) {
        return false;
    }
    return player_state_previous();
}

bool player_control_next()
{
    if (!player_control_stop_before_selection()) {
        return false;
    }
    return player_state_next();
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
    if (!player_control_stop_before_selection()) {
        return false;
    }
    return player_control_select_context(player_state_select_all_tracks(position));
}

bool player_control_select_artist_group(size_t group_index, size_t position)
{
    if (!player_control_stop_before_selection()) {
        return false;
    }
    return player_control_select_context(player_state_select_artist_group(group_index, position));
}

bool player_control_select_album_group(size_t group_index, size_t position)
{
    if (!player_control_stop_before_selection()) {
        return false;
    }
    return player_control_select_context(player_state_select_album_group(group_index, position));
}

bool player_control_select_decade_group(size_t group_index, size_t position)
{
    if (!player_control_stop_before_selection()) {
        return false;
    }
    return player_control_select_context(player_state_select_decade_group(group_index, position));
}
