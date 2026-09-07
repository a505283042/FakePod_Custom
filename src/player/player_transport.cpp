#include "player_transport.h"

#include <atomic>

#include "esp_log.h"
#include "esp_random.h"
#include "audio_service.h"
#include "media_library.h"
#include "player_state.h"
#include "app_diag_config.h"

static const char *TAG = "播放传输";

static std::atomic<uint8_t> g_loop_mode{
    static_cast<uint8_t>(PlayerLoopMode::Sequential)
};
static uint32_t g_last_audio_state_revision = 0U;
static uint32_t g_last_finished_playback_revision = 0U;

// P1.5.3.2R.8：随机播放只维护一个很小的“实际播放历史”，不生成整张 Shuffle Bag。
// 这样不会按曲库规模分配内存；上一曲可以回到随机模式下真正听过的上一首。
constexpr uint8_t kShuffleHistoryCapacity = 16U;
struct ShuffleHistory
{
    bool valid = false;
    PlayerFolderScope preferred_scope = PlayerFolderScope::All;
    PlayerFolderScope effective_scope = PlayerFolderScope::All;
    uint32_t catalog_generation = 0U;
    uint32_t context_id = 0U;
    uint32_t track_count = 0U;
    uint32_t positions[kShuffleHistoryCapacity] = {};
    uint8_t count = 0U;
};

static ShuffleHistory g_shuffle_history = {};

static bool player_transport_shuffle_context_matches(const PlayerFolderQueueSnapshot &list)
{
    return g_shuffle_history.valid &&
        g_shuffle_history.preferred_scope == list.preferred_scope &&
        g_shuffle_history.effective_scope == list.effective_scope &&
        g_shuffle_history.catalog_generation == list.catalog_generation &&
        g_shuffle_history.context_id == list.context_id &&
        g_shuffle_history.track_count == list.track_count;
}

static void player_transport_shuffle_reset(const PlayerFolderQueueSnapshot *list = nullptr)
{
    g_shuffle_history = {};
    if (list == nullptr || !list->ready) {
        return;
    }
    g_shuffle_history.valid = true;
    g_shuffle_history.preferred_scope = list->preferred_scope;
    g_shuffle_history.effective_scope = list->effective_scope;
    g_shuffle_history.catalog_generation = list->catalog_generation;
    g_shuffle_history.context_id = list->context_id;
    g_shuffle_history.track_count = list->track_count;
}

static void player_transport_shuffle_ensure_context(const PlayerFolderQueueSnapshot &list)
{
    if (!player_transport_shuffle_context_matches(list)) {
        player_transport_shuffle_reset(&list);
    }
}

static bool player_transport_shuffle_history_contains(uint32_t position)
{
    for (uint8_t i = 0U; i < g_shuffle_history.count; ++i) {
        if (g_shuffle_history.positions[i] == position) {
            return true;
        }
    }
    return false;
}

static void player_transport_shuffle_push(uint32_t position)
{
    if (g_shuffle_history.count > 0U &&
        g_shuffle_history.positions[g_shuffle_history.count - 1U] == position) {
        return;
    }

    if (g_shuffle_history.count >= kShuffleHistoryCapacity) {
        for (uint8_t i = 1U; i < kShuffleHistoryCapacity; ++i) {
            g_shuffle_history.positions[i - 1U] = g_shuffle_history.positions[i];
        }
        g_shuffle_history.count = kShuffleHistoryCapacity - 1U;
    }
    g_shuffle_history.positions[g_shuffle_history.count++] = position;
}

static bool player_transport_shuffle_pop(uint32_t *out_position)
{
    if (out_position == nullptr || g_shuffle_history.count == 0U) {
        return false;
    }
    --g_shuffle_history.count;
    *out_position = g_shuffle_history.positions[g_shuffle_history.count];
    return true;
}

static uint32_t player_transport_shuffle_choose_position(const PlayerFolderQueueSnapshot &list)
{
    if (list.track_count <= 1U) {
        return 0U;
    }

    // R2 刚选择新目录且当前歌曲不在该目录时，没有“当前队列位置”需要避开；
    // 此时随机可以从全部候选中任选。
    if (!list.current_in_queue) {
        for (uint8_t attempt = 0U; attempt < 12U; ++attempt) {
            const uint32_t candidate = esp_random() % list.track_count;
            if (!player_transport_shuffle_history_contains(candidate)) {
                return candidate;
            }
        }
        return esp_random() % list.track_count;
    }

    // 优先避开“当前首 + 最近随机历史”。列表很小时若历史占满候选，再退化为只避开当前首。
    for (uint8_t attempt = 0U; attempt < 12U; ++attempt) {
        const uint32_t candidate = esp_random() % list.track_count;
        if (candidate != list.position && !player_transport_shuffle_history_contains(candidate)) {
            return candidate;
        }
    }
    for (uint8_t attempt = 0U; attempt < 12U; ++attempt) {
        const uint32_t candidate = esp_random() % list.track_count;
        if (candidate != list.position) {
            return candidate;
        }
    }

    // 理论上只在极端随机碰撞时走到这里；保证不会立即重复当前首。
    return (list.position + 1U + (esp_random() % (list.track_count - 1U))) % list.track_count;
}

const char *player_transport_loop_mode_name(PlayerLoopMode mode)
{
    switch (mode) {
        case PlayerLoopMode::Sequential: return "顺序";
        case PlayerLoopMode::ListRepeat: return "列表循环";
        case PlayerLoopMode::SingleRepeat: return "单曲循环";
        case PlayerLoopMode::Shuffle: return "随机";
    }
    return "未知";
}

PlayerLoopMode player_transport_get_loop_mode()
{
    const uint8_t raw = g_loop_mode.load(std::memory_order_relaxed);
    if (raw > static_cast<uint8_t>(PlayerLoopMode::Shuffle)) {
        return PlayerLoopMode::Sequential;
    }
    return static_cast<PlayerLoopMode>(raw);
}

bool player_transport_set_loop_mode(PlayerLoopMode mode)
{
    if (static_cast<uint8_t>(mode) > static_cast<uint8_t>(PlayerLoopMode::Shuffle)) {
        return false;
    }

    const PlayerLoopMode previous = player_transport_get_loop_mode();
    g_loop_mode.store(static_cast<uint8_t>(mode), std::memory_order_relaxed);

    if (mode == PlayerLoopMode::Shuffle && previous != PlayerLoopMode::Shuffle) {
        PlayerFolderQueueSnapshot list = {};
        player_transport_shuffle_reset(
            player_state_get_folder_queue_snapshot(&list) ? &list : nullptr);
    } else if (mode != PlayerLoopMode::Shuffle) {
        player_transport_shuffle_reset();
    }

    ESP_LOGI(TAG, "播放模式：%s", player_transport_loop_mode_name(mode));
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
            next = PlayerLoopMode::Shuffle;
            break;
        case PlayerLoopMode::Shuffle:
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

#if APP_DIAG_PLAYER_TRANSPORT
    ESP_LOGI(TAG, "播放当前歌曲：原因=%s track=%u 路径=%s",
        reason != nullptr ? reason : "显式播放",
        static_cast<unsigned>(track_index),
        path);
#else
    (void)reason;
#endif

    return audio_service_play_track(
        static_cast<uint32_t>(track_index),
        path,
        player_state_get_format(),
        has_technical_info ? &technical : nullptr,
        false
    );
}

static bool player_transport_shuffle_next(const char *reason)
{
    PlayerFolderQueueSnapshot list = {};
    if (!player_state_get_folder_queue_snapshot(&list) || list.track_count == 0U) {
        ESP_LOGW(TAG, "随机下一首：当前目录播放范围不可用");
        return false;
    }

    player_transport_shuffle_ensure_context(list);
    if (list.track_count == 1U) {
        if (!list.current_in_queue && !player_state_select_folder_queue_position(0U)) {
            return false;
        }
        return player_transport_play_current(reason != nullptr ? reason : "随机单曲重播");
    }

    const uint32_t next_position = player_transport_shuffle_choose_position(list);
    if (list.current_in_queue) {
        player_transport_shuffle_push(list.position);
    }
    if (!player_state_select_folder_queue_position(next_position)) {
        ESP_LOGW(TAG, "随机下一首：选择目录队列 position=%lu 失败",
            static_cast<unsigned long>(next_position));
        return false;
    }
    return player_transport_play_current(reason != nullptr ? reason : "随机下一首");
}

bool player_transport_previous()
{
    AudioStateSnapshot snapshot = {};
    const bool has_audio = audio_service_get_snapshot(&snapshot) && snapshot.ready;
    const size_t current_track = player_state_get_index();

    // Stage 11.0 后，>3 秒上一曲直接走统一 Seek(0)，列表位置保持不变。
    // AudioTask 内部仍通过 pop-free 重建 pipeline，避免在 I2S 正在运行时硬改 Source。
    if (
        has_audio &&
        snapshot.track_index == current_track &&
        (snapshot.state == AudioPlaybackState::Playing || snapshot.state == AudioPlaybackState::Paused) &&
        snapshot.position_ms >= 3000ULL
    ) {
#if APP_DIAG_PLAYER_TRANSPORT
        ESP_LOGI(TAG, "手动上一曲：当前已播放超过3秒，Seek回曲首");
#endif
        return player_transport_seek_ms(0);
    }

    if (player_transport_get_loop_mode() == PlayerLoopMode::Shuffle) {
        PlayerFolderQueueSnapshot list = {};
        if (!player_state_get_folder_queue_snapshot(&list) || list.track_count == 0U) {
            return false;
        }
        player_transport_shuffle_ensure_context(list);

        uint32_t previous_position = 0U;
        if (player_transport_shuffle_pop(&previous_position)) {
            if (!player_state_select_folder_queue_position(previous_position)) {
                ESP_LOGW(TAG, "随机上一首：目录队列历史 position=%lu 已不可用",
                    static_cast<unsigned long>(previous_position));
                return false;
            }
            return player_transport_play_current("随机上一首历史");
        }
        // 本次随机会话还没有历史时，保留传统“上一首”的可预测回退。
    }

    if (!player_state_previous()) {
        ESP_LOGW(TAG, "手动上一曲：当前播放列表无法移动");
        return false;
    }
    return player_transport_play_current("手动上一曲");
}

bool player_transport_next()
{
    if (player_transport_get_loop_mode() == PlayerLoopMode::Shuffle) {
        return player_transport_shuffle_next("手动随机下一首");
    }

    if (!player_state_next()) {
        ESP_LOGW(TAG, "手动下一曲：当前播放列表无法移动");
        return false;
    }
    return player_transport_play_current("手动下一曲");
}


bool player_transport_seek_ms(uint64_t target_ms)
{
    if (!player_state_is_ready()) {
        return false;
    }
    const char *path = player_state_get_path();
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    const size_t track_index = player_state_get_index();
    MediaTechnicalInfo technical = {};
    const bool has_technical = media_library_get_technical_info(track_index, &technical);
#if APP_DIAG_AUDIO_SEEK
    ESP_LOGI(TAG, "SEEK_TRACE: Player请求 track=%u target=%llums",
        static_cast<unsigned>(track_index),
        static_cast<unsigned long long>(target_ms));
#endif
    return audio_service_seek_track(
        static_cast<uint32_t>(track_index),
        path,
        player_state_get_format(),
        has_technical ? &technical : nullptr,
        target_ms,
        false);
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

    PlayerFolderQueueSnapshot list = {};
    if (!player_state_get_folder_queue_snapshot(&list) || list.track_count == 0U) {
        ESP_LOGW(TAG, "AUTO_NEXT_TRACE: EOF 后目录播放范围不可用，停止自动续播");
        return;
    }

    // 若用户刚好在 EOF 边沿切换了 UI 列表，旧 AudioTask 的 Finished 不能推进新列表。
    if (audio.track_index == UINT32_MAX || audio.track_index != list.track_index) {
#if APP_DIAG_PLAYER_TRANSPORT
        ESP_LOGI(TAG,
            "AUTO_NEXT_TRACE: 忽略过期 EOF audio_track=%lu list_track=%lu generation=%lu",
            static_cast<unsigned long>(audio.track_index),
            static_cast<unsigned long>(list.track_index),
            static_cast<unsigned long>(list.catalog_generation));
#endif
        return;
    }

    const PlayerLoopMode mode = player_transport_get_loop_mode();
#if APP_DIAG_PLAYER_TRANSPORT
    ESP_LOGI(TAG,
        "AUTO_NEXT_TRACE: EOF mode=%s scope=%s effective=%s pos=%lu/%lu in_queue=%u track=%lu playback_rev=%lu",
        player_transport_loop_mode_name(mode),
        player_playlist_folder_scope_name(list.preferred_scope),
        player_playlist_folder_scope_name(list.effective_scope),
        static_cast<unsigned long>(list.current_in_queue ? list.position + 1U : 0U),
        static_cast<unsigned long>(list.track_count),
        list.current_in_queue ? 1U : 0U,
        static_cast<unsigned long>(list.track_index),
        static_cast<unsigned long>(audio.playback_revision));
#endif

    if (mode == PlayerLoopMode::SingleRepeat) {
        if (!player_transport_play_current("单曲循环 EOF")) {
            ESP_LOGE(TAG, "AUTO_NEXT_TRACE: 单曲循环重播请求失败");
        }
        return;
    }

    if (mode == PlayerLoopMode::Shuffle) {
        if (!player_transport_shuffle_next("随机播放 EOF")) {
            ESP_LOGE(TAG, "AUTO_NEXT_TRACE: 随机下一首请求失败");
        }
        return;
    }

    if (mode == PlayerLoopMode::Sequential &&
        list.current_in_queue && list.position + 1U >= list.track_count) {
#if APP_DIAG_PLAYER_TRANSPORT
        ESP_LOGI(TAG, "AUTO_NEXT_TRACE: 顺序播放已到列表末尾，保持 Finished");
#endif
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
