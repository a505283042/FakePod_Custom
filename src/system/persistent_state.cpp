#include "persistent_state.h"

#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "audio_service.h"
#include "device_settings.h"
#include "media_groups_v2.h"
#include "media_library.h"
#include "player_control.h"
#include "player_playlist.h"
#include "player_state.h"
#include "player_transport.h"

static const char *TAG = "持久化状态";

static constexpr uint16_t kSchemaVersion = 1U;
static constexpr const char *kSystemNamespace = "system";
static constexpr const char *kAudioNamespace = "audio";
static constexpr const char *kResumeNamespace = "resume";

struct PersistentResumeData
{
    bool valid = false;
    PlayerListType list_type = PlayerListType::AllTracks;
    char *track_path = nullptr;
    char *artist = nullptr;
    char *album_title = nullptr;
    char *album_artist = nullptr;
    uint16_t decade = 0U;
    bool decade_unknown = false;
};

struct PersistentStateData
{
    bool ready = false;
    bool schema_valid = false;
    bool has_volume = false;
    bool has_loop_mode = false;
    uint8_t volume = 50U;
    PlayerLoopMode loop_mode = PlayerLoopMode::Sequential;
    PersistentResumeData resume = {};
    uint32_t dirty_bits = PERSISTENT_DIRTY_NONE;
};

static PersistentStateData g_state = {};

static void persistent_free_string(char **value)
{
    if (value != nullptr && *value != nullptr) {
        heap_caps_free(*value);
        *value = nullptr;
    }
}

static void persistent_resume_clear(PersistentResumeData *resume)
{
    if (resume == nullptr) {
        return;
    }
    persistent_free_string(&resume->track_path);
    persistent_free_string(&resume->artist);
    persistent_free_string(&resume->album_title);
    persistent_free_string(&resume->album_artist);
    *resume = {};
}

static bool persistent_string_equal(const char *a, const char *b)
{
    const char *left = a != nullptr ? a : "";
    const char *right = b != nullptr ? b : "";
    return strcmp(left, right) == 0;
}

static bool persistent_replace_string(char **target, const char *value)
{
    if (target == nullptr) {
        return false;
    }
    const char *source = value != nullptr ? value : "";
    if (persistent_string_equal(*target, source)) {
        return true;
    }

    const size_t bytes = strlen(source) + 1U;
    char *replacement = static_cast<char *>(heap_caps_malloc(
        bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (replacement == nullptr) {
        ESP_LOGE(TAG, "PSRAM 分配失败：字符串=%uB", static_cast<unsigned>(bytes));
        return false;
    }
    memcpy(replacement, source, bytes);
    persistent_free_string(target);
    *target = replacement;
    return true;
}

static esp_err_t persistent_nvs_read_string(nvs_handle_t handle, const char *key, char **out_value)
{
    if (key == nullptr || out_value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t required = 0U;
    esp_err_t ret = nvs_get_str(handle, key, nullptr, &required);
    if (ret != ESP_OK) {
        return ret;
    }
    if (required == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buffer = static_cast<char *>(heap_caps_malloc(
        required, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    ret = nvs_get_str(handle, key, buffer, &required);
    if (ret != ESP_OK) {
        heap_caps_free(buffer);
        return ret;
    }
    persistent_free_string(out_value);
    *out_value = buffer;
    return ESP_OK;
}

static bool persistent_list_type_valid(uint8_t raw)
{
    return raw <= static_cast<uint8_t>(PlayerListType::Decade);
}

static bool persistent_loop_mode_valid(uint8_t raw)
{
    return raw <= static_cast<uint8_t>(PlayerLoopMode::Shuffle);
}

static esp_err_t persistent_load_audio()
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kAudioNamespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t volume = 0U;
    ret = nvs_get_u8(handle, "volume", &volume);
    if (ret == ESP_OK && volume <= 100U) {
        g_state.volume = volume;
        g_state.has_volume = true;
    } else if (ret != ESP_ERR_NVS_NOT_FOUND && ret != ESP_OK) {
        ESP_LOGW(TAG, "读取音量失败：%s", esp_err_to_name(ret));
    }

    uint8_t loop_mode = 0U;
    ret = nvs_get_u8(handle, "loop_mode", &loop_mode);
    if (ret == ESP_OK && persistent_loop_mode_valid(loop_mode)) {
        g_state.loop_mode = static_cast<PlayerLoopMode>(loop_mode);
        g_state.has_loop_mode = true;
    } else if (ret != ESP_ERR_NVS_NOT_FOUND && ret != ESP_OK) {
        ESP_LOGW(TAG, "读取播放模式失败：%s", esp_err_to_name(ret));
    }

    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t persistent_load_resume()
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kResumeNamespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t list_type_raw = 0U;
    ret = nvs_get_u8(handle, "list_type", &list_type_raw);
    if (ret != ESP_OK || !persistent_list_type_valid(list_type_raw)) {
        nvs_close(handle);
        return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : (ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE);
    }

    PersistentResumeData loaded = {};
    loaded.list_type = static_cast<PlayerListType>(list_type_raw);
    ret = persistent_nvs_read_string(handle, "track_path", &loaded.track_path);
    if (ret != ESP_OK || loaded.track_path == nullptr || loaded.track_path[0] == '\0') {
        persistent_resume_clear(&loaded);
        nvs_close(handle);
        return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : (ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE);
    }

    switch (loaded.list_type) {
        case PlayerListType::AllTracks:
            break;

        case PlayerListType::Artist:
            ret = persistent_nvs_read_string(handle, "artist", &loaded.artist);
            if (ret != ESP_OK || loaded.artist == nullptr || loaded.artist[0] == '\0') {
                persistent_resume_clear(&loaded);
                nvs_close(handle);
                return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
            }
            break;

        case PlayerListType::Album:
            ret = persistent_nvs_read_string(handle, "album_title", &loaded.album_title);
            if (ret == ESP_OK) {
                ret = persistent_nvs_read_string(handle, "album_artist", &loaded.album_artist);
            }
            if (ret != ESP_OK || loaded.album_title == nullptr || loaded.album_title[0] == '\0') {
                persistent_resume_clear(&loaded);
                nvs_close(handle);
                return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
            }
            break;

        case PlayerListType::Decade:
        {
            uint8_t unknown = 0U;
            ret = nvs_get_u16(handle, "decade", &loaded.decade);
            if (ret == ESP_OK) {
                ret = nvs_get_u8(handle, "dec_unknown", &unknown);
            }
            if (ret != ESP_OK || unknown > 1U) {
                persistent_resume_clear(&loaded);
                nvs_close(handle);
                return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
            }
            loaded.decade_unknown = unknown != 0U;
            break;
        }
    }

    loaded.valid = true;
    persistent_resume_clear(&g_state.resume);
    g_state.resume = loaded;
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t persistent_state_init()
{
    persistent_resume_clear(&g_state.resume);
    g_state = {};
    g_state.volume = 50U;
    g_state.loop_mode = PlayerLoopMode::Sequential;

    // 不自动 nvs_flash_erase()：未来 Wi-Fi/其他 APP 也会共享默认 NVS 分区，
    // 初始化异常时只禁用本轮持久化，避免为播放器设置破坏别的 namespace。
    const esp_err_t init_ret = nvs_flash_init();
    if (init_ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败，不执行自动擦除：%s", esp_err_to_name(init_ret));
        return init_ret;
    }

    nvs_handle_t system_handle = 0;
    esp_err_t ret = nvs_open(kSystemNamespace, NVS_READONLY, &system_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // NVS 驱动已就绪，只是尚无本项目的 namespace；持久化能力可用。
        g_state.ready = true;
        ESP_LOGI(TAG, "NVS V1 尚无持久化数据，使用运行时默认值");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t schema = 0U;
    ret = nvs_get_u16(system_handle, "schema_ver", &schema);
    nvs_close(system_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        g_state.ready = true;
        ESP_LOGI(TAG, "NVS V1 尚无 schema，使用运行时默认值");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (schema != kSchemaVersion) {
        g_state.ready = true;
        ESP_LOGW(TAG, "NVS schema 不匹配：stored=%u expected=%u，忽略旧状态",
            static_cast<unsigned>(schema), static_cast<unsigned>(kSchemaVersion));
        return ESP_OK;
    }

    g_state.schema_valid = true;
    const esp_err_t audio_ret = persistent_load_audio();
    if (audio_ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS audio namespace 读取失败：%s", esp_err_to_name(audio_ret));
    }
    const esp_err_t resume_ret = persistent_load_resume();
    if (resume_ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS resume namespace 读取失败：%s", esp_err_to_name(resume_ret));
    }

    g_state.ready = true;
    ESP_LOGI(TAG, "NVS V1 已加载：volume=%s loop=%s resume=%s",
        g_state.has_volume ? "YES" : "NO",
        g_state.has_loop_mode ? "YES" : "NO",
        g_state.resume.valid ? "YES" : "NO");
    return ESP_OK;
}

bool persistent_state_restore_audio()
{
    if (!g_state.ready || !g_state.schema_valid || !g_state.has_volume ||
        !device_settings_remember_volume_enabled()) {
        return true;
    }
    if (!audio_service_set_volume(g_state.volume, true)) {
        ESP_LOGW(TAG, "恢复音量失败：%u%%", static_cast<unsigned>(g_state.volume));
        return false;
    }
    ESP_LOGI(TAG, "恢复音量：%u%%", static_cast<unsigned>(g_state.volume));
    return true;
}

static bool persistent_find_track_by_path(const char *path, size_t *out_track_index)
{
    if (path == nullptr || path[0] == '\0' || out_track_index == nullptr) {
        return false;
    }

    // MusicCatalogV2 已按路径做不区分大小写严格排序；启动恢复可直接二分查找，
    // 不随未来曲库规模线性放大。运行时仍只返回当前 generation 的 Track index。
    size_t lo = 0U;
    size_t hi = media_library_get_count();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        const char *candidate = media_library_get_path(mid);
        if (candidate == nullptr) {
            return false;
        }
        const int cmp = strcasecmp(candidate, path);
        if (cmp < 0) {
            lo = mid + 1U;
        } else if (cmp > 0) {
            hi = mid;
        } else {
            *out_track_index = mid;
            return true;
        }
    }
    return false;
}

static bool persistent_find_track_position(
    const uint32_t *indices, uint32_t count, size_t track_index, size_t *out_position)
{
    if (indices == nullptr || out_position == nullptr) {
        return false;
    }
    for (uint32_t i = 0U; i < count; ++i) {
        if (indices[i] == track_index) {
            *out_position = static_cast<size_t>(i);
            return true;
        }
    }
    return false;
}

static bool persistent_restore_context(size_t track_index)
{
    size_t position = 0U;
    switch (g_state.resume.list_type) {
        case PlayerListType::AllTracks:
            return player_state_select_all_tracks(track_index);

        case PlayerListType::Artist:
            for (size_t group = 0U; group < media_groups_v2_artist_count(); ++group) {
                MediaArtistGroupViewV2 view = {};
                if (media_groups_v2_get_artist(group, &view) &&
                    persistent_string_equal(view.name, g_state.resume.artist) &&
                    persistent_find_track_position(view.track_indices, view.track_count, track_index, &position)) {
                    return player_state_select_artist_group(group, position);
                }
            }
            break;

        case PlayerListType::Album:
            for (size_t group = 0U; group < media_groups_v2_album_count(); ++group) {
                MediaAlbumGroupViewV2 view = {};
                if (media_groups_v2_get_album(group, &view) &&
                    persistent_string_equal(view.title, g_state.resume.album_title) &&
                    persistent_string_equal(view.artist, g_state.resume.album_artist) &&
                    persistent_find_track_position(view.track_indices, view.track_count, track_index, &position)) {
                    return player_state_select_album_group(group, position);
                }
            }
            break;

        case PlayerListType::Decade:
            for (size_t group = 0U; group < media_groups_v2_decade_count(); ++group) {
                MediaDecadeGroupViewV2 view = {};
                if (media_groups_v2_get_decade(group, &view) &&
                    view.decade_start == g_state.resume.decade &&
                    view.unknown == g_state.resume.decade_unknown &&
                    persistent_find_track_position(view.track_indices, view.track_count, track_index, &position)) {
                    return player_state_select_decade_group(group, position);
                }
            }
            break;
    }
    return false;
}

bool persistent_state_restore_player()
{
    if (!g_state.ready) {
        return false;
    }

    bool ok = true;
    if (g_state.schema_valid && g_state.has_loop_mode) {
        if (!player_control_set_loop_mode(g_state.loop_mode)) {
            ESP_LOGW(TAG, "恢复播放模式失败：%u", static_cast<unsigned>(g_state.loop_mode));
            ok = false;
        }
    }

    if (!g_state.schema_valid || !g_state.resume.valid || !player_state_is_ready() ||
        media_library_get_count() == 0U) {
        return ok;
    }

    size_t track_index = 0U;
    if (!persistent_find_track_by_path(g_state.resume.track_path, &track_index)) {
        ESP_LOGW(TAG, "恢复歌曲已不存在，保留默认选择：%s", g_state.resume.track_path);
        return false;
    }

    if (!persistent_restore_context(track_index)) {
        ESP_LOGW(TAG, "原播放列表身份已失效，降级恢复到全部歌曲：%s", g_state.resume.track_path);
        if (!player_state_select_all_tracks(track_index)) {
            return false;
        }
    }

    ESP_LOGI(TAG, "恢复当前歌曲：列表=%s path=%s（00:00，保持暂停）",
        player_playlist_type_name(player_state_get_list_type()), g_state.resume.track_path);
    return ok;
}

static bool persistent_capture_resume()
{
    if (!player_state_is_ready() || media_library_get_count() == 0U) {
        // TF 临时缺失/音乐库不可用时不能把旧恢复记录误判为“用户清空”。
        return true;
    }

    PlayerListSnapshot list = {};
    if (!player_state_get_list_snapshot(&list) || list.track_index == UINT32_MAX) {
        return true;
    }
    const char *path = media_library_get_path(list.track_index);
    if (path == nullptr || path[0] == '\0') {
        return true;
    }

    const char *artist = "";
    const char *album_title = "";
    const char *album_artist = "";
    uint16_t decade = 0U;
    bool decade_unknown = false;

    switch (list.type) {
        case PlayerListType::AllTracks:
            break;

        case PlayerListType::Artist:
        {
            MediaArtistGroupViewV2 view = {};
            if (!media_groups_v2_get_artist(list.group_index, &view) ||
                view.generation != list.catalog_generation || view.name == nullptr) {
                return false;
            }
            artist = view.name;
            break;
        }

        case PlayerListType::Album:
        {
            MediaAlbumGroupViewV2 view = {};
            if (!media_groups_v2_get_album(list.group_index, &view) ||
                view.generation != list.catalog_generation || view.title == nullptr || view.artist == nullptr) {
                return false;
            }
            album_title = view.title;
            album_artist = view.artist;
            break;
        }

        case PlayerListType::Decade:
            decade = list.decade_start;
            decade_unknown = list.decade_unknown;
            break;
    }

    const bool changed = !g_state.resume.valid ||
        g_state.resume.list_type != list.type ||
        !persistent_string_equal(g_state.resume.track_path, path) ||
        !persistent_string_equal(g_state.resume.artist, artist) ||
        !persistent_string_equal(g_state.resume.album_title, album_title) ||
        !persistent_string_equal(g_state.resume.album_artist, album_artist) ||
        g_state.resume.decade != decade ||
        g_state.resume.decade_unknown != decade_unknown;
    if (!changed) {
        return true;
    }

    if (!persistent_replace_string(&g_state.resume.track_path, path) ||
        !persistent_replace_string(&g_state.resume.artist, artist) ||
        !persistent_replace_string(&g_state.resume.album_title, album_title) ||
        !persistent_replace_string(&g_state.resume.album_artist, album_artist)) {
        return false;
    }
    g_state.resume.valid = true;
    g_state.resume.list_type = list.type;
    g_state.resume.decade = decade;
    g_state.resume.decade_unknown = decade_unknown;
    g_state.dirty_bits |= PERSISTENT_DIRTY_RESUME;
    return true;
}

void persistent_state_observe_runtime()
{
    if (!g_state.ready) {
        return;
    }

    if (device_settings_remember_volume_enabled()) {
        AudioStateSnapshot audio = {};
        if (audio_service_get_snapshot(&audio) && audio.ready &&
            (!g_state.has_volume || g_state.volume != audio.volume_percent)) {
            g_state.volume = audio.volume_percent;
            g_state.has_volume = true;
            g_state.dirty_bits |= PERSISTENT_DIRTY_AUDIO;
        }
    }

    const PlayerLoopMode loop_mode = player_control_get_loop_mode();
    if (!g_state.has_loop_mode || g_state.loop_mode != loop_mode) {
        g_state.loop_mode = loop_mode;
        g_state.has_loop_mode = true;
        g_state.dirty_bits |= PERSISTENT_DIRTY_AUDIO;
    }

    if (!persistent_capture_resume()) {
        ESP_LOGW(TAG, "RAM 恢复快照捕获失败，本轮不改写旧 resume 状态");
    }
}

static esp_err_t persistent_erase_key_if_present(nvs_handle_t handle, const char *key)
{
    const esp_err_t ret = nvs_erase_key(handle, key);
    return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : ret;
}

static esp_err_t persistent_flush_audio()
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kAudioNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    if (g_state.has_volume) {
        ret = nvs_set_u8(handle, "volume", g_state.volume);
    }
    if (ret == ESP_OK && g_state.has_loop_mode) {
        ret = nvs_set_u8(handle, "loop_mode", static_cast<uint8_t>(g_state.loop_mode));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t persistent_flush_resume()
{
    if (!g_state.resume.valid || g_state.resume.track_path == nullptr || g_state.resume.track_path[0] == '\0') {
        return ESP_OK;
    }

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kResumeNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(handle, "track_path", g_state.resume.track_path);
    if (ret == ESP_OK) ret = nvs_set_u8(handle, "list_type", static_cast<uint8_t>(g_state.resume.list_type));

    switch (g_state.resume.list_type) {
        case PlayerListType::AllTracks:
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "artist");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "album_title");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "album_artist");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "decade");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "dec_unknown");
            break;

        case PlayerListType::Artist:
            if (ret == ESP_OK) ret = nvs_set_str(handle, "artist", g_state.resume.artist != nullptr ? g_state.resume.artist : "");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "album_title");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "album_artist");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "decade");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "dec_unknown");
            break;

        case PlayerListType::Album:
            if (ret == ESP_OK) ret = nvs_set_str(handle, "album_title", g_state.resume.album_title != nullptr ? g_state.resume.album_title : "");
            if (ret == ESP_OK) ret = nvs_set_str(handle, "album_artist", g_state.resume.album_artist != nullptr ? g_state.resume.album_artist : "");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "artist");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "decade");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "dec_unknown");
            break;

        case PlayerListType::Decade:
            if (ret == ESP_OK) ret = nvs_set_u16(handle, "decade", g_state.resume.decade);
            if (ret == ESP_OK) ret = nvs_set_u8(handle, "dec_unknown", g_state.resume.decade_unknown ? 1U : 0U);
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "artist");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "album_title");
            if (ret == ESP_OK) ret = persistent_erase_key_if_present(handle, "album_artist");
            break;
    }

    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t persistent_flush_schema()
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kSystemNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u16(handle, "schema_ver", kSchemaVersion);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t persistent_state_flush()
{
    if (!g_state.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    // flush 是唯一 Flash 写入口；先捕获调用瞬间的最新 RAM 状态。
    persistent_state_observe_runtime();
    const uint32_t dirty = g_state.dirty_bits;
    if (dirty == PERSISTENT_DIRTY_NONE) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    if ((dirty & PERSISTENT_DIRTY_AUDIO) != 0U) {
        ret = persistent_flush_audio();
    }
    if (ret == ESP_OK && (dirty & PERSISTENT_DIRTY_RESUME) != 0U) {
        ret = persistent_flush_resume();
    }
    // schema 最后提交：第一次写入中途掉电时，没有 schema 的半成品不会在下次启动被恢复。
    if (ret == ESP_OK && !g_state.schema_valid) {
        ret = persistent_flush_schema();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "显式 NVS flush 失败：dirty=0x%08lX %s",
            static_cast<unsigned long>(dirty), esp_err_to_name(ret));
        return ret;
    }

    g_state.schema_valid = true;
    g_state.dirty_bits &= ~dirty;
    ESP_LOGI(TAG, "显式 NVS flush 完成：dirty=0x%08lX", static_cast<unsigned long>(dirty));
    return ESP_OK;
}

bool persistent_state_get_status(PersistentStateStatus *out_status)
{
    if (out_status == nullptr) {
        return false;
    }
    PersistentStateStatus status = {};
    status.ready = g_state.ready;
    status.schema_valid = g_state.schema_valid;
    status.has_volume = g_state.has_volume;
    status.has_loop_mode = g_state.has_loop_mode;
    status.has_resume = g_state.resume.valid;
    status.dirty_bits = g_state.dirty_bits;
    *out_status = status;
    return true;
}
