#include "nas_catalog_service.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_service.h"
#include "media_catalog_store_v2.h"
#include "storage_io.h"
#include "system_paths.h"
#include "wifi_service.h"

static const char *TAG = "NAS索引";

namespace {

static constexpr size_t kBaseUrlMax = 256U;
static constexpr size_t kUsernameMax = 64U;
static constexpr size_t kPasswordMax = 96U;
static constexpr size_t kUrlMax = 384U;
static constexpr size_t kMetaBufferBytes = 1024U;
static constexpr size_t kIoBufferBytes = 4096U;
static constexpr uint32_t kMaxTracks = 100000U;
static constexpr uint32_t kMaxIndexBytes = 48U * 1024U * 1024U;
static constexpr uint32_t kMaxManifestBytes = 4U * 1024U * 1024U;
static constexpr uint32_t kWorkerStackBytes = 5120U;
static constexpr UBaseType_t kWorkerPriority = 2U;
static constexpr BaseType_t kWorkerCore = 1;
static constexpr int kHttpTimeoutMs = 5000;

static constexpr const char *kRemoteMetaName = "fakepod_catalog.meta";
static constexpr const char *kRemoteIndexName = "music_index_v2.bin";
static constexpr const char *kRemoteManifestName = "music_manifest_v2.bin";

struct NasConfig
{
    char base_url[kBaseUrlMax] = {};
    char track_url[kBaseUrlMax] = {};
    char music_url[kBaseUrlMax] = {};
    char username[kUsernameMax] = {};
    char password[kPasswordMax] = {};
};

struct NasRemoteMeta
{
    uint32_t catalog_version = 0U;
    uint64_t revision = 0ULL;
    uint32_t track_count = 0U;
    uint32_t index_size = 0U;
    uint32_t index_crc32 = 0U;
    uint32_t manifest_size = 0U;
    uint32_t manifest_crc32 = 0U;
    uint32_t short_id_version = 0U;
};

struct NasSyncContext
{
    NasConfig config = {};
    NasRemoteMeta remote = {};
    char meta_text[kMetaBufferBytes] = {};
    char meta_parse[kMetaBufferBytes] = {};
    char url[kUrlMax] = {};
    uint8_t io_buffer[kIoBufferBytes] = {};
};

static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static bool g_ready = false;
static bool g_configured = false;
static bool g_cached = false;
static bool g_syncing = false;
static NasCatalogState g_state = NasCatalogState::Unconfigured;
static esp_err_t g_last_error = ESP_OK;
static uint32_t g_track_count = 0U;
static uint64_t g_revision = 0ULL;

static void publish_state(
    NasCatalogState state,
    esp_err_t error,
    bool configured,
    bool cached,
    bool syncing,
    uint32_t tracks,
    uint64_t revision)
{
    taskENTER_CRITICAL(&g_lock);
    g_ready = true;
    g_state = state;
    g_last_error = error;
    g_configured = configured;
    g_cached = cached;
    g_syncing = syncing;
    g_track_count = tracks;
    g_revision = revision;
    taskEXIT_CRITICAL(&g_lock);
}

static char *trim(char *text)
{
    if (text == nullptr) return nullptr;
    while (*text != '\0' && isspace(static_cast<unsigned char>(*text))) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace(static_cast<unsigned char>(end[-1]))) --end;
    *end = '\0';
    return text;
}

static bool parse_u32(const char *text, uint32_t *out)
{
    if (text == nullptr || out == nullptr || text[0] == '\0') return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *trim(end) != '\0' || value > UINT32_MAX) return false;
    *out = static_cast<uint32_t>(value);
    return true;
}

static bool parse_u64(const char *text, uint64_t *out)
{
    if (text == nullptr || out == nullptr || text[0] == '\0') return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long long value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *trim(end) != '\0') return false;
    *out = static_cast<uint64_t>(value);
    return true;
}

static bool file_exists(const char *path)
{
    struct stat info = {};
    return path != nullptr && stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static bool file_size_matches(const char *path, uint32_t expected)
{
    struct stat info = {};
    return path != nullptr && stat(path, &info) == 0 && info.st_size >= 0 &&
        static_cast<uint64_t>(info.st_size) == expected;
}

static esp_err_t read_small_file(const char *path, char *buffer, size_t buffer_size)
{
    if (path == nullptr || buffer == nullptr || buffer_size < 2U) return ESP_ERR_INVALID_ARG;
    StorageSdLockGuard sd_lock(pdMS_TO_TICKS(1000));
    if (!sd_lock.locked()) return ESP_ERR_TIMEOUT;
    FILE *file = fopen(path, "rb");
    if (file == nullptr) return ESP_ERR_NOT_FOUND;
    const size_t used = fread(buffer, 1, buffer_size - 1U, file);
    const int extra = fgetc(file);
    fclose(file);
    if (extra != EOF) return ESP_ERR_INVALID_SIZE;
    buffer[used] = '\0';
    return ESP_OK;
}

static esp_err_t write_small_file(const char *path, const char *text)
{
    if (path == nullptr || text == nullptr) return ESP_ERR_INVALID_ARG;
    StorageSdLockGuard sd_lock(pdMS_TO_TICKS(1000));
    if (!sd_lock.locked()) return ESP_ERR_TIMEOUT;
    FILE *file = fopen(path, "wb");
    if (file == nullptr) return ESP_FAIL;
    const size_t size = strlen(text);
    bool ok = fwrite(text, 1, size, file) == size && fflush(file) == 0;
    if (ok) {
        const int fd = fileno(file);
        ok = fd < 0 || fsync(fd) == 0;
    }
    fclose(file);
    return ok ? ESP_OK : ESP_FAIL;
}

static bool parse_config_text(char *text, NasConfig *config)
{
    if (text == nullptr || config == nullptr) return false;
    *config = {};
    char *save = nullptr;
    for (char *line = strtok_r(text, "\r\n", &save); line != nullptr; line = strtok_r(nullptr, "\r\n", &save)) {
        line = trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char *equals = strchr(line, '=');
        if (equals == nullptr) continue;
        *equals = '\0';
        char *key = trim(line);
        char *value = trim(equals + 1);
        if (strcmp(key, "base_url") == 0) {
            snprintf(config->base_url, sizeof(config->base_url), "%s", value);
        } else if (strcmp(key, "track_url") == 0) {
            snprintf(config->track_url, sizeof(config->track_url), "%s", value);
        } else if (strcmp(key, "music_url") == 0) {
            snprintf(config->music_url, sizeof(config->music_url), "%s", value);
        } else if (strcmp(key, "username") == 0) {
            snprintf(config->username, sizeof(config->username), "%s", value);
        } else if (strcmp(key, "password") == 0) {
            snprintf(config->password, sizeof(config->password), "%s", value);
        }
    }
    size_t length = strlen(config->base_url);
    while (length > 0U && config->base_url[length - 1U] == '/') {
        config->base_url[--length] = '\0';
    }
    length = strlen(config->track_url);
    while (length > 0U && config->track_url[length - 1U] == '/') {
        config->track_url[--length] = '\0';
    }
    length = strlen(config->music_url);
    while (length > 0U && config->music_url[length - 1U] == '/') {
        config->music_url[--length] = '\0';
    }
    // R46.0.70 刻意只开放 HTTP：不拉入 TLS 运行期开销；HTTPS 后续必须单独量 RAM 后再决定。
    if (strncmp(config->base_url, "http://", 7U) != 0 || strlen(config->base_url) <= 7U) {
        return false;
    }
    const bool track_url_ok = config->track_url[0] == '\0' ||
        (strncmp(config->track_url, "http://", 7U) == 0 && strlen(config->track_url) > 7U);
    const bool music_url_ok = config->music_url[0] == '\0' ||
        (strncmp(config->music_url, "http://", 7U) == 0 && strlen(config->music_url) > 7U);
    return track_url_ok && music_url_ok;
}

static bool parse_remote_meta(char *text, NasRemoteMeta *meta)
{
    if (text == nullptr || meta == nullptr) return false;
    *meta = {};
    bool magic_ok = false;
    char *save = nullptr;
    for (char *line = strtok_r(text, "\r\n", &save); line != nullptr; line = strtok_r(nullptr, "\r\n", &save)) {
        line = trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (strcmp(line, "FAKEPOD_NAS_V1") == 0) {
            magic_ok = true;
            continue;
        }
        char *equals = strchr(line, '=');
        if (equals == nullptr) continue;
        *equals = '\0';
        char *key = trim(line);
        char *value = trim(equals + 1);
        if (strcmp(key, "catalog_version") == 0) {
            (void)parse_u32(value, &meta->catalog_version);
        } else if (strcmp(key, "revision") == 0) {
            (void)parse_u64(value, &meta->revision);
        } else if (strcmp(key, "tracks") == 0) {
            (void)parse_u32(value, &meta->track_count);
        } else if (strcmp(key, "index_size") == 0) {
            (void)parse_u32(value, &meta->index_size);
        } else if (strcmp(key, "index_crc32") == 0) {
            (void)parse_u32(value, &meta->index_crc32);
        } else if (strcmp(key, "manifest_size") == 0) {
            (void)parse_u32(value, &meta->manifest_size);
        } else if (strcmp(key, "manifest_crc32") == 0) {
            (void)parse_u32(value, &meta->manifest_crc32);
        } else if (strcmp(key, "track_alias_version") == 0 || strcmp(key, "track_map_version") == 0) {
            uint32_t version = 0U;
            if (parse_u32(value, &version) && version > meta->short_id_version) {
                meta->short_id_version = version;
            }
        }
    }
    return magic_ok && meta->catalog_version == 6U && meta->revision != 0ULL &&
        meta->track_count <= kMaxTracks && meta->index_size > 0U && meta->index_size <= kMaxIndexBytes &&
        meta->manifest_size >= 32U && meta->manifest_size <= kMaxManifestBytes;
}

static bool build_url(const NasConfig &config, const char *name, char *out, size_t out_size)
{
    if (name == nullptr || out == nullptr || out_size == 0U) return false;
    const int written = snprintf(out, out_size, "%s/%s", config.base_url, name);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

static esp_http_client_handle_t http_open_get(const NasConfig &config, const char *url, esp_err_t *out_error)
{
    esp_http_client_config_t http = {};
    http.url = url;
    http.timeout_ms = kHttpTimeoutMs;
    http.buffer_size = 512;
    // Authenticated WebDAV GET has an Authorization header. Increase only the
    // short-lived index sync client's TX buffer; normal unauthenticated HTTP stays unchanged.
    http.buffer_size_tx = config.username[0] != '\0' ? 512 : 256;
    http.keep_alive_enable = false;
    if (config.username[0] != '\0') {
        http.username = config.username;
        http.password = config.password;
        http.auth_type = HTTP_AUTH_TYPE_BASIC;
    }
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (client == nullptr) {
        if (out_error != nullptr) *out_error = ESP_ERR_NO_MEM;
        return nullptr;
    }
    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) != 200) {
            ret = ESP_FAIL;
        }
    }
    if (ret != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (out_error != nullptr) *out_error = ret;
        return nullptr;
    }
    if (out_error != nullptr) *out_error = ESP_OK;
    return client;
}

static esp_err_t http_get_text(const NasConfig &config, const char *url, char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size < 2U) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ESP_OK;
    esp_http_client_handle_t client = http_open_get(config, url, &ret);
    if (client == nullptr) return ret;
    size_t used = 0U;
    while (used + 1U < buffer_size) {
        const int got = esp_http_client_read(client, buffer + used, buffer_size - used - 1U);
        if (got < 0) {
            ret = ESP_FAIL;
            break;
        }
        if (got == 0) break;
        used += static_cast<size_t>(got);
    }
    if (ret == ESP_OK && used + 1U == buffer_size) {
        char extra = 0;
        if (esp_http_client_read(client, &extra, 1U) > 0) ret = ESP_ERR_INVALID_SIZE;
    }
    buffer[used] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

static void remove_file_locked(const char *path)
{
    StorageSdLockGuard sd_lock(pdMS_TO_TICKS(1000));
    if (sd_lock.locked()) (void)remove(path);
}

static esp_err_t http_download_file(
    const NasConfig &config,
    const char *url,
    const char *path,
    uint32_t expected_size,
    uint8_t *io_buffer,
    size_t io_size)
{
    if (path == nullptr || io_buffer == nullptr || io_size == 0U) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ESP_OK;
    esp_http_client_handle_t client = http_open_get(config, url, &ret);
    if (client == nullptr) return ret;

    const int64_t content_length = esp_http_client_get_content_length(client);
    if (content_length >= 0 && static_cast<uint64_t>(content_length) != expected_size) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    StorageSdLockGuard sd_lock(pdMS_TO_TICKS(2000));
    if (!sd_lock.locked()) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_TIMEOUT;
    }
    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint32_t total = 0U;
    while (ret == ESP_OK) {
        const int got = esp_http_client_read(client, reinterpret_cast<char *>(io_buffer), io_size);
        if (got < 0) {
            ret = ESP_FAIL;
            break;
        }
        if (got == 0) break;
        if (static_cast<uint32_t>(got) > expected_size - total ||
            fwrite(io_buffer, 1, static_cast<size_t>(got), file) != static_cast<size_t>(got)) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        total += static_cast<uint32_t>(got);
    }
    if (ret == ESP_OK && total != expected_size) ret = ESP_ERR_INVALID_SIZE;
    if (ret == ESP_OK && fflush(file) != 0) ret = ESP_FAIL;
    if (ret == ESP_OK) {
        const int fd = fileno(file);
        if (fd >= 0 && fsync(fd) != 0) ret = ESP_FAIL;
    }
    fclose(file);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (ret != ESP_OK) (void)remove(path);
    return ret;
}

static bool audio_idle_for_catalog_sync()
{
    AudioStateSnapshot audio = {};
    if (!audio_service_get_snapshot(&audio)) return false;
    switch (audio.state) {
        case AudioPlaybackState::Ready:
        case AudioPlaybackState::Finished:
        case AudioPlaybackState::Stopped:
        case AudioPlaybackState::Error:
            return true;
        default:
            return false;
    }
}

static esp_err_t validate_pair(
    const char *index_path,
    const char *manifest_path,
    const NasRemoteMeta &expected,
    uint32_t *out_tracks)
{
    if (!file_size_matches(index_path, expected.index_size) ||
        !file_size_matches(manifest_path, expected.manifest_size)) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t tracks = 0U;
    uint32_t index_crc = 0U;
    uint32_t manifest_crc = 0U;
    const esp_err_t ret = media_catalog_store_v2_validate_pair_files(
        index_path, manifest_path, &tracks, &index_crc, &manifest_crc);
    if (ret != ESP_OK) return ret;
    const bool matches = tracks == expected.track_count &&
        index_crc == expected.index_crc32 && manifest_crc == expected.manifest_crc32;
    if (out_tracks != nullptr) *out_tracks = tracks;
    return matches ? ESP_OK : ESP_ERR_INVALID_CRC;
}

static esp_err_t commit_downloads(const char *meta_text)
{
    esp_err_t ret = write_small_file(SystemPaths::kNasCatalogMetaTemp, meta_text);
    if (ret != ESP_OK) return ret;

    StorageSdLockGuard sd_lock(pdMS_TO_TICKS(2000));
    if (!sd_lock.locked()) return ESP_ERR_TIMEOUT;

    (void)remove(SystemPaths::kNasMusicIndexV2Backup);
    (void)remove(SystemPaths::kNasMusicManifestV2Backup);
    (void)remove(SystemPaths::kNasCatalogMetaBackup);

    bool index_backed = rename(SystemPaths::kNasMusicIndexV2, SystemPaths::kNasMusicIndexV2Backup) == 0;
    if (!index_backed && errno != ENOENT) return ESP_FAIL;
    bool manifest_backed = rename(SystemPaths::kNasMusicManifestV2, SystemPaths::kNasMusicManifestV2Backup) == 0;
    if (!manifest_backed && errno != ENOENT) {
        if (index_backed) (void)rename(SystemPaths::kNasMusicIndexV2Backup, SystemPaths::kNasMusicIndexV2);
        return ESP_FAIL;
    }
    bool meta_backed = rename(SystemPaths::kNasCatalogMeta, SystemPaths::kNasCatalogMetaBackup) == 0;
    if (!meta_backed && errno != ENOENT) {
        if (manifest_backed) (void)rename(SystemPaths::kNasMusicManifestV2Backup, SystemPaths::kNasMusicManifestV2);
        if (index_backed) (void)rename(SystemPaths::kNasMusicIndexV2Backup, SystemPaths::kNasMusicIndexV2);
        return ESP_FAIL;
    }

    const bool index_ok = rename(SystemPaths::kNasMusicIndexV2Temp, SystemPaths::kNasMusicIndexV2) == 0;
    const bool manifest_ok = index_ok &&
        rename(SystemPaths::kNasMusicManifestV2Temp, SystemPaths::kNasMusicManifestV2) == 0;
    const bool meta_ok = manifest_ok &&
        rename(SystemPaths::kNasCatalogMetaTemp, SystemPaths::kNasCatalogMeta) == 0;
    if (meta_ok) return ESP_OK;

    (void)remove(SystemPaths::kNasMusicIndexV2);
    (void)remove(SystemPaths::kNasMusicManifestV2);
    (void)remove(SystemPaths::kNasCatalogMeta);
    if (meta_backed) (void)rename(SystemPaths::kNasCatalogMetaBackup, SystemPaths::kNasCatalogMeta);
    if (manifest_backed) (void)rename(SystemPaths::kNasMusicManifestV2Backup, SystemPaths::kNasMusicManifestV2);
    if (index_backed) (void)rename(SystemPaths::kNasMusicIndexV2Backup, SystemPaths::kNasMusicIndexV2);
    return ESP_FAIL;
}

static esp_err_t load_config(NasSyncContext *ctx)
{
    if (ctx == nullptr) return ESP_ERR_INVALID_ARG;
    char *buffer = static_cast<char *>(heap_caps_malloc(kMetaBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) return ESP_ERR_NO_MEM;
    const esp_err_t ret = read_small_file(SystemPaths::kNasCatalogConfig, buffer, kMetaBufferBytes);
    bool ok = false;
    if (ret == ESP_OK) ok = parse_config_text(buffer, &ctx->config);
    heap_caps_free(buffer);
    if (ret != ESP_OK) return ret;
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void finish_worker(NasSyncContext *ctx, NasCatalogState state, esp_err_t error,
    bool cached, uint32_t tracks, uint64_t revision)
{
    const UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG,
        "同步结束：state=%s tracks=%lu error=%s stack_hwm=%u internal=%u min=%u psram=%u",
        nas_catalog_service_state_name(state), static_cast<unsigned long>(tracks), esp_err_to_name(error),
        static_cast<unsigned>(stack_hwm),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    publish_state(state, error, true, cached, false, tracks, revision);
    heap_caps_free(ctx);
    vTaskDelete(nullptr);
}

static void sync_worker(void *arg)
{
    NasSyncContext *ctx = static_cast<NasSyncContext *>(arg);
    if (ctx == nullptr) {
        publish_state(NasCatalogState::Failed, ESP_ERR_INVALID_ARG, true, false, false, 0U, 0ULL);
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t ret = load_config(ctx);
    if (ret != ESP_OK) {
        finish_worker(ctx, NasCatalogState::Failed, ret, false, 0U, 0ULL);
        return;
    }

    if (!build_url(ctx->config, kRemoteMetaName, ctx->url, sizeof(ctx->url))) {
        finish_worker(ctx, NasCatalogState::Failed, ESP_ERR_INVALID_SIZE, false, 0U, 0ULL);
        return;
    }
    ret = http_get_text(ctx->config, ctx->url, ctx->meta_text, sizeof(ctx->meta_text));
    if (ret != ESP_OK) {
        finish_worker(ctx, NasCatalogState::Failed, ret, false, 0U, 0ULL);
        return;
    }

    snprintf(ctx->meta_parse, sizeof(ctx->meta_parse), "%s", ctx->meta_text);
    if (!parse_remote_meta(ctx->meta_parse, &ctx->remote)) {
        finish_worker(ctx, NasCatalogState::Failed, ESP_ERR_INVALID_RESPONSE, false, 0U, 0ULL);
        return;
    }

    char *local_text = static_cast<char *>(heap_caps_malloc(kMetaBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (local_text == nullptr) {
        finish_worker(ctx, NasCatalogState::Failed, ESP_ERR_NO_MEM, false, 0U, 0ULL);
        return;
    }
    NasRemoteMeta local_meta = {};
    esp_err_t local_meta_ret = read_small_file(SystemPaths::kNasCatalogMeta, local_text, kMetaBufferBytes);
    if (local_meta_ret == ESP_OK) {
        char *local_parse = static_cast<char *>(heap_caps_malloc(kMetaBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (local_parse != nullptr) {
            snprintf(local_parse, kMetaBufferBytes, "%s", local_text);
            if (!parse_remote_meta(local_parse, &local_meta)) local_meta = {};
            heap_caps_free(local_parse);
        }
    }
    heap_caps_free(local_text);

    if (local_meta.revision == ctx->remote.revision && local_meta.revision != 0ULL) {
        uint32_t tracks = 0U;
        ret = validate_pair(SystemPaths::kNasMusicIndexV2, SystemPaths::kNasMusicManifestV2,
            ctx->remote, &tracks);
        if (ret == ESP_OK) {
            finish_worker(ctx, NasCatalogState::UpToDate, ESP_OK, true, tracks, ctx->remote.revision);
            return;
        }
        ESP_LOGW(TAG, "本地 NAS Catalog revision 命中但校验失败：%s；重新下载", esp_err_to_name(ret));
    }

    remove_file_locked(SystemPaths::kNasMusicIndexV2Temp);
    remove_file_locked(SystemPaths::kNasMusicManifestV2Temp);
    remove_file_locked(SystemPaths::kNasCatalogMetaTemp);

    if (!build_url(ctx->config, kRemoteIndexName, ctx->url, sizeof(ctx->url))) {
        finish_worker(ctx, NasCatalogState::Failed, ESP_ERR_INVALID_SIZE, false, 0U, 0ULL);
        return;
    }
    ret = http_download_file(ctx->config, ctx->url, SystemPaths::kNasMusicIndexV2Temp,
        ctx->remote.index_size, ctx->io_buffer, sizeof(ctx->io_buffer));
    if (ret != ESP_OK) {
        finish_worker(ctx, NasCatalogState::Failed, ret, false, 0U, 0ULL);
        return;
    }

    if (!build_url(ctx->config, kRemoteManifestName, ctx->url, sizeof(ctx->url))) {
        finish_worker(ctx, NasCatalogState::Failed, ESP_ERR_INVALID_SIZE, false, 0U, 0ULL);
        return;
    }
    ret = http_download_file(ctx->config, ctx->url, SystemPaths::kNasMusicManifestV2Temp,
        ctx->remote.manifest_size, ctx->io_buffer, sizeof(ctx->io_buffer));
    if (ret != ESP_OK) {
        remove_file_locked(SystemPaths::kNasMusicIndexV2Temp);
        finish_worker(ctx, NasCatalogState::Failed, ret, false, 0U, 0ULL);
        return;
    }

    uint32_t tracks = 0U;
    ret = validate_pair(SystemPaths::kNasMusicIndexV2Temp, SystemPaths::kNasMusicManifestV2Temp,
        ctx->remote, &tracks);
    if (ret != ESP_OK) {
        remove_file_locked(SystemPaths::kNasMusicIndexV2Temp);
        remove_file_locked(SystemPaths::kNasMusicManifestV2Temp);
        finish_worker(ctx, NasCatalogState::Failed, ret, false, 0U, 0ULL);
        return;
    }

    ret = commit_downloads(ctx->meta_text);
    if (ret != ESP_OK) {
        finish_worker(ctx, NasCatalogState::Failed, ret, false, 0U, 0ULL);
        return;
    }
    finish_worker(ctx, NasCatalogState::Updated, ESP_OK, true, tracks, ctx->remote.revision);
}

} // namespace

esp_err_t nas_catalog_service_init()
{
    NasCatalogSnapshot existing = {};
    if (nas_catalog_service_get_snapshot(&existing) && existing.ready) {
        return ESP_OK;
    }

    char *text = static_cast<char *>(heap_caps_malloc(kMetaBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (text == nullptr) return ESP_ERR_NO_MEM;

    bool configured = false;
    {
        StorageSdLockGuard sd_lock(pdMS_TO_TICKS(1000));
        configured = sd_lock.locked() && file_exists(SystemPaths::kNasCatalogConfig);
    }

    NasRemoteMeta local = {};
    const esp_err_t meta_ret = read_small_file(SystemPaths::kNasCatalogMeta, text, kMetaBufferBytes);
    if (meta_ret == ESP_OK) {
        char *copy = static_cast<char *>(heap_caps_malloc(kMetaBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (copy != nullptr) {
            snprintf(copy, kMetaBufferBytes, "%s", text);
            if (!parse_remote_meta(copy, &local)) local = {};
            heap_caps_free(copy);
        }
    }
    heap_caps_free(text);

    const bool cached = local.revision != 0ULL &&
        file_size_matches(SystemPaths::kNasMusicIndexV2, local.index_size) &&
        file_size_matches(SystemPaths::kNasMusicManifestV2, local.manifest_size);
    publish_state(configured ? NasCatalogState::Idle : NasCatalogState::Unconfigured,
        ESP_OK, configured, cached, false, cached ? local.track_count : 0U, cached ? local.revision : 0ULL);
    ESP_LOGI(TAG, "Foundation就绪：配置=%s 缓存=%s tracks=%lu（未联网、未常驻Catalog）",
        configured ? "有" : "无", cached ? "有" : "无",
        static_cast<unsigned long>(cached ? local.track_count : 0U));
    return ESP_OK;
}

esp_err_t nas_catalog_service_request_sync()
{
    NasCatalogSnapshot snapshot = {};
    if (!nas_catalog_service_get_snapshot(&snapshot)) return ESP_ERR_INVALID_STATE;
    if (snapshot.syncing) return ESP_ERR_INVALID_STATE;
    if (!snapshot.configured) return ESP_ERR_NOT_FOUND;

    WifiServiceSnapshot wifi = {};
    if (!wifi_service_get_snapshot(&wifi) || !wifi.connected) return ESP_ERR_INVALID_STATE;
    if (!audio_idle_for_catalog_sync()) return ESP_ERR_INVALID_STATE;

    NasSyncContext *ctx = static_cast<NasSyncContext *>(
        heap_caps_calloc(1, sizeof(NasSyncContext), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (ctx == nullptr) return ESP_ERR_NO_MEM;

    publish_state(NasCatalogState::Syncing, ESP_OK, true, snapshot.cached, true,
        snapshot.track_count, snapshot.revision);
    BaseType_t created = xTaskCreatePinnedToCore(
        sync_worker, "NasCatalog", kWorkerStackBytes, ctx, kWorkerPriority, nullptr, kWorkerCore);
    if (created != pdPASS) {
        heap_caps_free(ctx);
        publish_state(NasCatalogState::Failed, ESP_ERR_NO_MEM, true, snapshot.cached, false,
            snapshot.track_count, snapshot.revision);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "开始同步：worker_stack=%uB meta=%uB io=%uB（meta/io均在PSRAM）",
        static_cast<unsigned>(kWorkerStackBytes), static_cast<unsigned>(kMetaBufferBytes),
        static_cast<unsigned>(kIoBufferBytes));
    return ESP_OK;
}

bool nas_catalog_service_get_snapshot(NasCatalogSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    taskENTER_CRITICAL(&g_lock);
    out_snapshot->ready = g_ready;
    out_snapshot->configured = g_configured;
    out_snapshot->cached = g_cached;
    out_snapshot->syncing = g_syncing;
    out_snapshot->state = g_state;
    out_snapshot->last_error = g_last_error;
    out_snapshot->track_count = g_track_count;
    out_snapshot->revision = g_revision;
    taskEXIT_CRITICAL(&g_lock);
    return out_snapshot->ready;
}



esp_err_t nas_catalog_service_get_playback_endpoint(NasPlaybackEndpoint *out_endpoint)
{
    if (out_endpoint == nullptr) return ESP_ERR_INVALID_ARG;
    *out_endpoint = {};

    char *text = static_cast<char *>(heap_caps_malloc(
        kMetaBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (text == nullptr) return ESP_ERR_NO_MEM;
    esp_err_t ret = read_small_file(SystemPaths::kNasCatalogConfig, text, kMetaBufferBytes);
    if (ret != ESP_OK) {
        heap_caps_free(text);
        return ret;
    }

    NasConfig config = {};
    if (!parse_config_text(text, &config)) {
        heap_caps_free(text);
        return ESP_ERR_INVALID_ARG;
    }

    NasRemoteMeta local_meta = {};
    if (read_small_file(SystemPaths::kNasCatalogMeta, text, kMetaBufferBytes) == ESP_OK) {
        (void)parse_remote_meta(text, &local_meta);
    }
    heap_caps_free(text);

    if (config.track_url[0] != '\0') {
        snprintf(out_endpoint->track_base_url, sizeof(out_endpoint->track_base_url), "%s", config.track_url);
    } else if (config.music_url[0] == '\0' && local_meta.short_id_version >= 1U) {
        // Only infer legacy short-ID aliases if no direct music_url is configured.
        // R46.0.117 WebDAV uses the existing V2 paths and does NOT require /track links.
        // DSM Web Station exposes /volume1/web as the HTTP document root.  Derive the
        // static short-ID endpoint from the HTTP origin, not from the catalog path; e.g.
        // http://host:8080/web/music-index -> http://host:8080/track.
        static constexpr const char *kIndexSuffix = "/music-index";
        const size_t base_len = strlen(config.base_url);
        const size_t suffix_len = strlen(kIndexSuffix);
        if (base_len > suffix_len &&
            strcasecmp(config.base_url + base_len - suffix_len, kIndexSuffix) == 0) {
            const char *scheme_end = strstr(config.base_url, "://");
            const char *path_begin = scheme_end != nullptr ? strchr(scheme_end + 3, '/') : nullptr;
            if (path_begin == nullptr) return ESP_ERR_INVALID_ARG;
            const size_t origin_len = static_cast<size_t>(path_begin - config.base_url);
            const int written = snprintf(
                out_endpoint->track_base_url, sizeof(out_endpoint->track_base_url),
                "%.*s/track", static_cast<int>(origin_len), config.base_url);
            if (written <= 0 || static_cast<size_t>(written) >= sizeof(out_endpoint->track_base_url)) {
                return ESP_ERR_INVALID_SIZE;
            }
        }
    }

    if (config.music_url[0] != '\0') {
        snprintf(out_endpoint->music_base_url, sizeof(out_endpoint->music_base_url), "%s", config.music_url);
    } else {
        static constexpr const char *kIndexSuffix = "/music-index";
        const size_t base_len = strlen(config.base_url);
        const size_t suffix_len = strlen(kIndexSuffix);
        if (base_len <= suffix_len || strcasecmp(config.base_url + base_len - suffix_len, kIndexSuffix) != 0) {
            ESP_LOGW(TAG, "NAS播放地址无法从base_url推导，请在nas_catalog.conf增加music_url=");
            return ESP_ERR_NOT_FOUND;
        }
        const size_t prefix_len = base_len - suffix_len;
        const int written = snprintf(
            out_endpoint->music_base_url,
            sizeof(out_endpoint->music_base_url),
            "%.*s/music",
            static_cast<int>(prefix_len),
            config.base_url);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(out_endpoint->music_base_url)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    snprintf(out_endpoint->username, sizeof(out_endpoint->username), "%s", config.username);
    snprintf(out_endpoint->password, sizeof(out_endpoint->password), "%s", config.password);
    return ESP_OK;
}

static uint64_t nas_track_id_fnv1a64(const char *relative_path)
{
    uint64_t value = 0xCBF29CE484222325ULL;
    if (relative_path == nullptr) return value;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(relative_path); *p != 0U; ++p) {
        value ^= static_cast<uint64_t>(*p);
        value *= 0x100000001B3ULL;
    }
    return value;
}

static bool nas_url_unreserved(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

esp_err_t nas_catalog_service_build_track_url(
    const NasPlaybackEndpoint *endpoint,
    const char *relative_path,
    char *out_url,
    size_t out_url_size)
{
    if (endpoint == nullptr || relative_path == nullptr || out_url == nullptr || out_url_size == 0U ||
        relative_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (endpoint->track_base_url[0] != '\0') {
        const char *slash = strrchr(relative_path, '/');
        const char *dot = strrchr(relative_path, '.');
        if (dot == nullptr || dot[1] == '\0' || (slash != nullptr && dot < slash)) {
            return ESP_ERR_INVALID_ARG;
        }
        const size_t ext_len = strlen(dot);
        if (ext_len < 2U || ext_len > 8U) return ESP_ERR_INVALID_ARG;
        for (size_t i = 1U; i < ext_len; ++i) {
            const unsigned char ch = static_cast<unsigned char>(dot[i]);
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) {
                return ESP_ERR_INVALID_ARG;
            }
        }

        const uint64_t track_id = nas_track_id_fnv1a64(relative_path);
        const int written = snprintf(
            out_url, out_url_size, "%s/%016llX%s", endpoint->track_base_url,
            static_cast<unsigned long long>(track_id), dot);
        return written > 0 && static_cast<size_t>(written) < out_url_size
            ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    if (endpoint->music_base_url[0] == '\0') return ESP_ERR_NOT_FOUND;
    size_t used = strlen(endpoint->music_base_url);
    if (used + 2U > out_url_size) return ESP_ERR_INVALID_SIZE;
    memcpy(out_url, endpoint->music_base_url, used);
    const char *path = relative_path;
    if (path[0] != '/') out_url[used++] = '/';

    static constexpr char kHex[] = "0123456789ABCDEF";
    for (; *path != '\0'; ++path) {
        const unsigned char ch = static_cast<unsigned char>(*path);
        if (ch == '/' || nas_url_unreserved(ch)) {
            if (used + 1U >= out_url_size) return ESP_ERR_INVALID_SIZE;
            out_url[used++] = static_cast<char>(ch);
        } else {
            if (used + 3U >= out_url_size) return ESP_ERR_INVALID_SIZE;
            out_url[used++] = '%';
            out_url[used++] = kHex[(ch >> 4U) & 0x0FU];
            out_url[used++] = kHex[ch & 0x0FU];
        }
    }
    out_url[used] = '\0';
    return ESP_OK;
}

const char *nas_catalog_service_state_name(NasCatalogState state)
{
    switch (state) {
        case NasCatalogState::Unconfigured: return "未配置";
        case NasCatalogState::Idle: return "就绪";
        case NasCatalogState::Syncing: return "同步中";
        case NasCatalogState::UpToDate: return "已是最新";
        case NasCatalogState::Updated: return "已更新";
        case NasCatalogState::Failed: return "同步失败";
    }
    return "未知";
}
