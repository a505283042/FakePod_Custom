#include "player_transport.h"

#include <atomic>

#include "esp_log.h"
#include "audio_service.h"
#include "media_library.h"
#include "player_state.h"

static const char *TAG = "播放传输";

static std::atomic<uint8_t> g_loop_mode{
    static_cast<uint8_t>(PlayerLoopMode::Sequential)
};
static uint32_t g_last_audio_state_revision = 0U;
static uint32_t g_last_finished_playback_revision = 0U;

const char *player_transport_loop_mode_name(PlayerLoopMode mode)
{
    switch (mode) {
        case PlayerLoopMode::Sequential: return "顺序";
        case PlayerLoopMode::ListRepeat: return "列表循环";
        case PlayerLoopMode::SingleRepeat: return "单曲循环";
    }
    return "未知";
}

PlayerLoopMode player_transport_get_loop_mode()
{
    const uint8_t raw = g_loop_mode.load(std::memory_order_relaxed);
    if (raw > static_cast<uint8_t>(PlayerLoopMode::SingleRepeat)) {
        return PlayerLoopMode::Sequential;
    }
    return static_cast<PlayerLoopMode>(raw);
}

bool player_transport_set_loop_mode(PlayerLoopMode mode)
{
    if (static_cast<uint8_t>(mode) > static_cast<uint8_t>(PlayerLoopMode::SingleRepeat)) {
        return false;
    }
    g_loop_mode.store(static_cast<uint8_t>(mode), std::memory_order_relaxed);
    ESP_LOGI(TAG, "循环模式：%s", player_transport_loop_mode_name(mode));
    return true;
}

PlayerLoopMode player_transport_cycle_loop_mode()
{
    PlayerLoopMode next = PlayerLoopMode::Sequential;
    switch (player_transport_get_loop_mode()) {
        case PlayerLoopMode::Sequential:
            next = PlayerLoopMode::ListRepeat;
            break;
        case PlayerLoopMode::ListRepeat:
            next = PlayerLoopMode::SingleRepeat;
            break;
        case PlayerLoopMode::SingleRepeat:
            next = PlayerLoopMode::Sequential;
            break;
    }
    player_transport_set_loop_mode(next);
    return next;
}

bool player_transport_play_current(const char *reason)
{
    if (!player_state_is_ready()) {
        ESP_LOGW(TAG, "播放当前歌曲失败：Player 尚未就绪");
        return false;
    }

    const char *path = player_state_get_path();
    if (path == nullptr || path[0] == '\0') {
        ESP_LOGW(TAG, "播放当前歌曲失败：当前列表没有有效歌曲");
        return false;
    }

    const size_t track_index = player_state_get_index();
    MediaTechnicalInfo technical = {};
    const bool has_technical_info = media_library_get_technical_info(track_index, &technical);

    ESP_LOGI(TAG, "播放当前歌曲：原因=%s track=%u 路径=%s",
        reason != nullptr ? reason : "显式播放",
        static_cast<unsigned>(track_index),
        path);

    return audio_service_play_track(
        static_cast<uint32_t>(track_index),
        path,
        player_state_get_format(),
        has_technical_info ? &technical : nullptr,
        false
    );
}

bool player_transport_previous()
{
    AudioStateSnapshot snapshot = {};
    const bool has_audio = audio_service_get_snapshot(&snapshot) && snapshot.ready;
    const size_t current_track = player_state_get_index();

    // 在 seek 尚未接入前，用“重新发 Play 当前曲”实现标准的 >3 秒回到曲首语义。
    // 这会经过现有 pop-free shutdown/start 序列，不直接修改 decoder 文件位置。
    if (
        has_audio &&
        snapshot.track_index == current_track &&
        (snapshot.state == AudioPlaybackState::Playing || snapshot.state == AudioPlaybackState::Paused) &&
        snapshot.position_ms >= 3000ULL
    ) {
        ESP_LOGI(TAG, "手动上一曲：当前已播放超过3秒，重播当前首");
        return player_transport_play_current("手动上一曲重播");
    }

    if (!player_state_previous()) {
        ESP_LOGW(TAG, "手动上一曲：当前播放列表无法移动");
        return false;
    }
    return player_transport_play_current("手动上一曲");
}

bool player_transport_next()
{
    if (!player_state_next()) {
        ESP_LOGW(TAG, "手动下一曲：当前播放列表无法移动");
        return false;
    }
    return player_transport_play_current("手动下一曲");
}

static void player_transport_handle_finished(const AudioStateSnapshot &audio)
{
    if (
        audio.playback_revision == 0U ||
        audio.playback_revision == g_last_finished_playback_revision
    ) {
        return;
    }
    g_last_finished_playback_revision = audio.playback_revision;

    PlayerListSnapshot list = {};
    if (!player_state_get_list_snapshot(&list) || list.track_count == 0U) {
        ESP_LOGW(TAG, "AUTO_NEXT_TRACE: EOF 后播放列表不可用，停止自动续播");
        return;
    }

    // 若用户刚好在 EOF 边沿切换了 UI 列表，旧 AudioTask 的 Finished 不能推进新列表。
    if (audio.track_index == UINT32_MAX || audio.track_index != list.track_index) {
        ESP_LOGI(TAG,
            "AUTO_NEXT_TRACE: 忽略过期 EOF audio_track=%lu list_track=%lu generation=%lu",
            static_cast<unsigned long>(audio.track_index),
            static_cast<unsigned long>(list.track_index),
            static_cast<unsigned long>(list.catalog_generation));
        return;
    }

    const PlayerLoopMode mode = player_transport_get_loop_mode();
    ESP_LOGI(TAG,
        "AUTO_NEXT_TRACE: EOF mode=%s list=%s pos=%lu/%lu track=%lu playback_rev=%lu",
        player_transport_loop_mode_name(mode),
        player_playlist_type_name(list.type),
        static_cast<unsigned long>(list.position + 1U),
        static_cast<unsigned long>(list.track_count),
        static_cast<unsigned long>(list.track_index),
        static_cast<unsigned long>(audio.playback_revision));

    if (mode == PlayerLoopMode::SingleRepeat) {
        if (!player_transport_play_current("单曲循环 EOF")) {
            ESP_LOGE(TAG, "AUTO_NEXT_TRACE: 单曲循环重播请求失败");
        }
        return;
    }

    if (mode == PlayerLoopMode::Sequential && list.position + 1U >= list.track_count) {
        ESP_LOGI(TAG, "AUTO_NEXT_TRACE: 顺序播放已到列表末尾，保持 Finished");
        return;
    }

    if (!player_state_next()) {
        ESP_LOGE(TAG, "AUTO_NEXT_TRACE: EOF 推进下一首失败");
        return;
    }

    if (!player_transport_play_current(
            mode == PlayerLoopMode::ListRepeat ? "列表循环 EOF" : "顺序播放 EOF")) {
        ESP_LOGE(TAG, "AUTO_NEXT_TRACE: 下一首播放请求失败");
    }
}

void player_transport_update()
{
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        return;
    }

    if (snapshot.state_revision == g_last_audio_state_revision) {
        return;
    }
    g_last_audio_state_revision = snapshot.state_revision;

    if (snapshot.state == AudioPlaybackState::Finished) {
        player_transport_handle_finished(snapshot);
    }
}
