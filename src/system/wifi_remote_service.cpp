#include "wifi_remote_service.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "audio_service.h"
#include "battery_service.h"
#include "media_catalog_v2.h"
#include "nas_library_source.h"
#include "player_control.h"
#include "video_app.h"
#include "visual_music_app.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "WiFi遥控";

namespace {

static constexpr uint16_t kDiscoveryPort = 46320U;
static constexpr uint16_t kControlPort = 46321U;
static constexpr char kDiscoveryQuery[] = "FAKEPOD_DISCOVER_V1";
static constexpr char kDiscoveryReply[] = "FAKEPOD_REMOTE_V1";
static constexpr uint8_t kFrameMagic = 0xA5U;
static constexpr uint8_t kProtocolVersion = 1U;
static constexpr size_t kStatusPacketSize = 20U;
static constexpr size_t kMetadataChunkBytes = 14U;
static constexpr size_t kMetadataMaxBytes = 196U;
static constexpr size_t kMetadataChunkCountMax =
    (kMetadataMaxBytes + kMetadataChunkBytes - 1U) / kMetadataChunkBytes;
static constexpr size_t kMetadataWireMaxBytes =
    kMetadataChunkCountMax * (2U + 6U + kMetadataChunkBytes);
static constexpr TickType_t kStatusInterval = pdMS_TO_TICKS(500);
static constexpr TickType_t kTaskStopTimeout = pdMS_TO_TICKS(1200);
// R46.0.59: runtime HWM ~=3.2KB free on 6144B stack; retain >2KB measured margin.
static constexpr uint32_t kTaskStackBytes = 5120U;
static constexpr UBaseType_t kTaskPriority = 2U;
static constexpr BaseType_t kTaskCore = 1;
static constexpr UBaseType_t kTaskStackCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

enum class MediaContext : uint8_t {
    Music = 0,
    Video,
    Nsf,
};

static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static bool g_running = false;
static bool g_stop_requested = false;
static bool g_start_complete = false;
static esp_err_t g_start_result = ESP_FAIL;
static TaskHandle_t g_task = nullptr;
static int g_udp_fd = -1;
static int g_listen_fd = -1;
static int g_client_fd = -1;

static char g_metadata[kMetadataMaxBytes + 1U] = {};
static size_t g_metadata_size = 0U;
static uint16_t g_metadata_sequence = 1U;
static uint32_t g_metadata_track = UINT32_MAX;
static uint32_t g_metadata_generation = 0U;
static uint32_t g_metadata_revision = 0U;
static MediaContext g_metadata_context = MediaContext::Music;
static bool g_metadata_initialized = false;
static AudioPlaybackSource g_metadata_music_source = AudioPlaybackSource::Local;


static void write_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = static_cast<uint8_t>(value & 0xFFU);
    dst[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

static void write_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = static_cast<uint8_t>(value & 0xFFU);
    dst[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    dst[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    dst[3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

static uint32_t clamp_u64_to_u32(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
}

static size_t trim_utf8_tail(char *text, size_t size)
{
    while (size > 0U && (static_cast<uint8_t>(text[size - 1U]) & 0xC0U) == 0x80U) --size;
    if (size > 0U && (static_cast<uint8_t>(text[size - 1U]) & 0x80U) != 0U) --size;
    text[size] = '\0';
    return size;
}

static void set_metadata(
    const char *title,
    const char *artist,
    uint32_t track,
    uint32_t generation,
    uint32_t revision,
    MediaContext context)
{
    const int written = snprintf(
        g_metadata,
        sizeof(g_metadata),
        "%s\n%s",
        title != nullptr ? title : "",
        artist != nullptr ? artist : "");
    if (written < 0) {
        g_metadata[0] = '\0';
        g_metadata_size = 0U;
    } else if (static_cast<size_t>(written) >= sizeof(g_metadata)) {
        g_metadata_size = trim_utf8_tail(g_metadata, kMetadataMaxBytes);
    } else {
        g_metadata_size = static_cast<size_t>(written);
    }
    ++g_metadata_sequence;
    if (g_metadata_sequence == 0U) ++g_metadata_sequence;
    g_metadata_track = track;
    g_metadata_generation = generation;
    g_metadata_revision = revision;
    g_metadata_context = context;
    g_metadata_initialized = true;
}

static void ensure_music_metadata(uint32_t track, AudioPlaybackSource source)
{
    MediaTrackViewV2 view = {};
    uint32_t generation = 0U;
    const char *title = "";
    const char *artist = "";
    const bool have_view = track != UINT32_MAX &&
        (source == AudioPlaybackSource::NasHttp
            ? nas_library_source_get_track_view(track, &view)
            : media_catalog_v2_get_track_view(track, &view));
    if (have_view) {
        generation = view.generation;
        if (view.title != nullptr) title = view.title;
        if (view.artist != nullptr) artist = view.artist;
    }
    if (!g_metadata_initialized || g_metadata_context != MediaContext::Music ||
        g_metadata_track != track || g_metadata_generation != generation ||
        g_metadata_music_source != source) {
        set_metadata(title, artist, track, generation, 0U, MediaContext::Music);
        g_metadata_music_source = source;
    }
}

static void ensure_video_metadata(const VideoRemoteSnapshot &video)
{
    if (!g_metadata_initialized || g_metadata_context != MediaContext::Video ||
        g_metadata_track != video.item_index || g_metadata_revision != video.metadata_revision) {
        set_metadata(video.title, "视频", video.item_index, 0U, video.metadata_revision, MediaContext::Video);
    }
}

static void ensure_nsf_metadata(const NsfRemoteSnapshot &nsf)
{
    if (!g_metadata_initialized || g_metadata_context != MediaContext::Nsf ||
        g_metadata_track != nsf.track_index || g_metadata_revision != nsf.metadata_revision) {
        set_metadata(nsf.title, nsf.artist, nsf.track_index, 0U, nsf.metadata_revision, MediaContext::Nsf);
    }
}

static void build_status(uint8_t packet[kStatusPacketSize])
{
    memset(packet, 0, kStatusPacketSize);
    packet[0] = 0x01U;
    packet[1] = kProtocolVersion;

    AudioStateSnapshot audio = {};
    const bool audio_valid = audio_service_get_snapshot(&audio) && audio.ready;
    NsfRemoteSnapshot nsf = {};
    const bool nsf_valid = visual_music_app_get_remote_snapshot(&nsf) && nsf.active;
    VideoRemoteSnapshot video = {};
    const bool video_valid = !nsf_valid && video_app_get_remote_snapshot(&video) && video.active;

    const uint32_t track = nsf_valid
        ? nsf.track_index
        : (video_valid ? video.item_index : (audio_valid ? audio.track_index : UINT32_MAX));
    if (nsf_valid) ensure_nsf_metadata(nsf);
    else if (video_valid) ensure_video_metadata(video);
    else ensure_music_metadata(
        track, audio_valid ? audio.source : AudioPlaybackSource::Local);

    packet[2] = nsf_valid
        ? static_cast<uint8_t>(nsf.paused
            ? AudioPlaybackState::Paused
            : (nsf.playing ? AudioPlaybackState::Playing : AudioPlaybackState::Preparing))
        : (video_valid
            ? static_cast<uint8_t>(video.paused
                ? AudioPlaybackState::Paused
                : (video.playing ? AudioPlaybackState::Playing : AudioPlaybackState::Preparing))
            : (audio_valid ? static_cast<uint8_t>(audio.state) : 0xFFU));
    packet[3] = audio_valid ? audio.volume_percent : 0U;

    BatterySnapshot battery = {};
    const bool battery_valid = battery_service_get_snapshot(&battery) && battery.valid;
    packet[4] = battery_valid ? battery.percent : 0xFFU;

    uint8_t flags = 0U;
    if (audio_valid && audio.user_muted) flags |= 0x01U;
    if (battery_valid) flags |= 0x02U;
    if (track != UINT32_MAX) flags |= 0x04U;
    if (video_valid) flags |= 0x08U;
    if (nsf_valid) flags |= 0x10U;
    packet[5] = flags;

    write_u32_le(&packet[6], track);
    write_u32_le(&packet[10], nsf_valid
        ? nsf.position_ms
        : (video_valid ? video.position_ms : (audio_valid ? clamp_u64_to_u32(audio.position_ms) : 0U)));

    uint64_t duration_ms = nsf_valid ? nsf.duration_ms : (video_valid ? video.duration_ms : 0ULL);
    if (!nsf_valid && !video_valid && audio_valid && audio.sample_rate_hz > 0U && audio.total_frames > 0ULL) {
        duration_ms = (audio.total_frames * 1000ULL) / audio.sample_rate_hz;
    }
    write_u32_le(&packet[14], clamp_u64_to_u32(duration_ms));
    write_u16_le(&packet[18], g_metadata_sequence);
}

static bool execute_command(const uint8_t *payload, size_t size)
{
    if (payload == nullptr || size == 0U) return false;
    const uint8_t command = payload[0];
    if (command >= 0x01U && command <= 0x03U && visual_music_app_remote_control_active()) {
        const NsfRemoteCommand nsf_command = command == 0x01U
            ? NsfRemoteCommand::TogglePlayPause
            : (command == 0x02U ? NsfRemoteCommand::Next : NsfRemoteCommand::Previous);
        return visual_music_app_remote_submit(nsf_command);
    }
    if (command >= 0x01U && command <= 0x03U && video_app_remote_control_active()) {
        const VideoRemoteCommand video_command = command == 0x01U
            ? VideoRemoteCommand::TogglePlayPause
            : (command == 0x02U ? VideoRemoteCommand::Next : VideoRemoteCommand::Previous);
        return video_app_remote_submit(video_command);
    }
    switch (command) {
        case 0x01U: return size == 1U && player_control_toggle_play_pause();
        case 0x02U: return size == 1U && player_control_next();
        case 0x03U: return size == 1U && player_control_previous();
        case 0x04U: return size == 1U && player_control_volume_up(1U);
        case 0x05U: return size == 1U && player_control_volume_down(1U);
        case 0x06U: return size == 2U && payload[1] <= 100U && player_control_set_volume(payload[1]);
        default: return false;
    }
}

static bool send_all(int fd, const uint8_t *data, size_t size)
{
    size_t offset = 0U;
    while (offset < size) {
        const int written = send(fd, data + offset, size - offset, 0);
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

static bool send_frame(int fd, const uint8_t *payload, size_t size)
{
    if (fd < 0 || payload == nullptr || size == 0U || size > 255U) return false;
    uint8_t frame[257] = {};
    frame[0] = kFrameMagic;
    frame[1] = static_cast<uint8_t>(size);
    memcpy(&frame[2], payload, size);
    return send_all(fd, frame, size + 2U);
}

static bool send_metadata(int fd)
{
    const uint8_t chunk_count = static_cast<uint8_t>(
        (g_metadata_size + kMetadataChunkBytes - 1U) / kMetadataChunkBytes);
    const uint8_t count = chunk_count == 0U ? 1U : chunk_count;
    uint8_t wire[kMetadataWireMaxBytes] = {};
    size_t wire_size = 0U;
    for (uint8_t index = 0U; index < count; ++index) {
        uint8_t packet[6U + kMetadataChunkBytes] = {};
        packet[0] = 0x02U;
        packet[1] = kProtocolVersion;
        write_u16_le(&packet[2], g_metadata_sequence);
        packet[4] = index;
        packet[5] = count;
        const size_t offset = static_cast<size_t>(index) * kMetadataChunkBytes;
        const size_t remaining = offset < g_metadata_size ? g_metadata_size - offset : 0U;
        const size_t chunk = remaining > kMetadataChunkBytes ? kMetadataChunkBytes : remaining;
        if (chunk > 0U) memcpy(&packet[6], &g_metadata[offset], chunk);

        const size_t payload_size = 6U + chunk;
        if (wire_size + 2U + payload_size > sizeof(wire)) return false;
        wire[wire_size++] = kFrameMagic;
        wire[wire_size++] = static_cast<uint8_t>(payload_size);
        memcpy(&wire[wire_size], packet, payload_size);
        wire_size += payload_size;
    }
    return send_all(fd, wire, wire_size);
}

static void close_fd(int &fd)
{
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        fd = -1;
    }
}

static int create_udp_socket()
{
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) return -1;
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kDiscoveryPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int create_listen_socket()
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return -1;
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kControlPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void service_task(void *)
{
    g_udp_fd = create_udp_socket();
    g_listen_fd = create_listen_socket();
    if (g_udp_fd < 0 || g_listen_fd < 0) {
        ESP_LOGE(TAG, "Wi-Fi遥控socket启动失败：udp=%d tcp=%d errno=%d", g_udp_fd, g_listen_fd, errno);
        close_fd(g_client_fd);
        close_fd(g_listen_fd);
        close_fd(g_udp_fd);
        portENTER_CRITICAL(&g_lock);
        g_start_result = ESP_FAIL;
        g_start_complete = true;
        g_running = false;
        g_task = nullptr;
        portEXIT_CRITICAL(&g_lock);
        vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
        return;
    }

    portENTER_CRITICAL(&g_lock);
    g_start_result = ESP_OK;
    g_start_complete = true;
    portEXIT_CRITICAL(&g_lock);
    ESP_LOGI(TAG, "Wi-Fi遥控已启动：UDP=%u TCP=%u protocol=V1", kDiscoveryPort, kControlPort);

    TickType_t status_due = 0;
    uint16_t sent_metadata_sequence = 0U;
    uint8_t rx_buffer[128] = {};
    size_t rx_size = 0U;

    for (;;) {
        portENTER_CRITICAL(&g_lock);
        const bool stop = g_stop_requested;
        portEXIT_CRITICAL(&g_lock);
        if (stop) break;

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(g_udp_fd, &read_set);
        FD_SET(g_listen_fd, &read_set);
        int max_fd = g_udp_fd > g_listen_fd ? g_udp_fd : g_listen_fd;
        if (g_client_fd >= 0) {
            FD_SET(g_client_fd, &read_set);
            if (g_client_fd > max_fd) max_fd = g_client_fd;
        }
        timeval timeout = {};
        timeout.tv_usec = 100000;
        const int ready = select(max_fd + 1, &read_set, nullptr, nullptr, &timeout);
        if (ready < 0 && errno != EINTR) {
            ESP_LOGW(TAG, "Wi-Fi遥控select失败：errno=%d", errno);
            break;
        }

        bool accepted_client = false;
        if (ready > 0 && FD_ISSET(g_udp_fd, &read_set)) {
            sockaddr_in source = {};
            socklen_t source_len = sizeof(source);
            char query[64] = {};
            const int received = recvfrom(
                g_udp_fd, query, sizeof(query) - 1U, 0,
                reinterpret_cast<sockaddr *>(&source), &source_len);
            if (received == static_cast<int>(strlen(kDiscoveryQuery)) &&
                memcmp(query, kDiscoveryQuery, strlen(kDiscoveryQuery)) == 0) {
                (void)sendto(
                    g_udp_fd,
                    kDiscoveryReply,
                    strlen(kDiscoveryReply),
                    0,
                    reinterpret_cast<sockaddr *>(&source),
                    source_len);
            }
        }

        if (ready > 0 && FD_ISSET(g_listen_fd, &read_set)) {
            sockaddr_in peer = {};
            socklen_t peer_len = sizeof(peer);
            const int client = accept(g_listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if (client >= 0) {
                close_fd(g_client_fd);
                g_client_fd = client;
                rx_size = 0U;
                sent_metadata_sequence = 0U;
                status_due = 0;
                char peer_text[16] = {};
                inet_ntoa_r(peer.sin_addr, peer_text, sizeof(peer_text));
                ESP_LOGI(TAG, "手机Wi-Fi遥控已连接：%s", peer_text);
                accepted_client = true;
            }
        }

        if (g_client_fd >= 0 && !accepted_client && ready > 0 && FD_ISSET(g_client_fd, &read_set)) {
            const int received = recv(
                g_client_fd,
                rx_buffer + rx_size,
                sizeof(rx_buffer) - rx_size,
                0);
            if (received <= 0) {
                ESP_LOGI(TAG, "手机Wi-Fi遥控已断开");
                close_fd(g_client_fd);
                rx_size = 0U;
            } else {
                rx_size += static_cast<size_t>(received);
                size_t consumed = 0U;
                while (rx_size - consumed >= 2U) {
                    if (rx_buffer[consumed] != kFrameMagic) {
                        ++consumed;
                        continue;
                    }
                    const size_t payload_size = rx_buffer[consumed + 1U];
                    if (payload_size == 0U || payload_size > 32U) {
                        ++consumed;
                        continue;
                    }
                    if (rx_size - consumed < payload_size + 2U) break;
                    const uint8_t *payload = &rx_buffer[consumed + 2U];
                    const bool ok = execute_command(payload, payload_size);
                    ESP_LOGI(TAG, "Wi-Fi手机控制：command=0x%02X result=%s",
                        static_cast<unsigned>(payload[0]), ok ? "OK" : "FAIL");
                    consumed += payload_size + 2U;
                }
                if (consumed > 0U) {
                    memmove(rx_buffer, rx_buffer + consumed, rx_size - consumed);
                    rx_size -= consumed;
                }
                if (rx_size == sizeof(rx_buffer)) rx_size = 0U;
            }
        }

        if (g_client_fd >= 0) {
            const TickType_t now = xTaskGetTickCount();
            if (status_due == 0 || static_cast<int32_t>(now - status_due) >= 0) {
                uint8_t status[kStatusPacketSize] = {};
                build_status(status);
                if (!send_frame(g_client_fd, status, sizeof(status))) {
                    ESP_LOGI(TAG, "手机Wi-Fi遥控发送失败，关闭TCP会话");
                    close_fd(g_client_fd);
                    rx_size = 0U;
                } else {
                    status_due = now + kStatusInterval;
                    if (sent_metadata_sequence != g_metadata_sequence) {
                        if (send_metadata(g_client_fd)) {
                            sent_metadata_sequence = g_metadata_sequence;
                        } else {
                            close_fd(g_client_fd);
                            rx_size = 0U;
                        }
                    }
                }
            }
        }
    }

    close_fd(g_client_fd);
    close_fd(g_listen_fd);
    close_fd(g_udp_fd);
    portENTER_CRITICAL(&g_lock);
    g_running = false;
    g_stop_requested = false;
    g_task = nullptr;
    portEXIT_CRITICAL(&g_lock);
    ESP_LOGI(TAG, "Wi-Fi遥控已停止");
    vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
}

} // namespace

esp_err_t wifi_remote_service_start()
{
    portENTER_CRITICAL(&g_lock);
    if (g_running) {
        portEXIT_CRITICAL(&g_lock);
        return ESP_OK;
    }
    g_running = true;
    g_stop_requested = false;
    g_start_complete = false;
    g_start_result = ESP_FAIL;
    g_metadata_initialized = false;
    portEXIT_CRITICAL(&g_lock);

    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        service_task,
        "wifi_remote",
        kTaskStackBytes,
        nullptr,
        kTaskPriority,
        &task,
        kTaskCore,
        kTaskStackCaps);
    if (created != pdPASS) {
        portENTER_CRITICAL(&g_lock);
        g_running = false;
        portEXIT_CRITICAL(&g_lock);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&g_lock);
    g_task = task;
    portEXIT_CRITICAL(&g_lock);

    const TickType_t started = xTaskGetTickCount();
    while (static_cast<TickType_t>(xTaskGetTickCount() - started) < pdMS_TO_TICKS(1000)) {
        portENTER_CRITICAL(&g_lock);
        const bool complete = g_start_complete;
        const esp_err_t result = g_start_result;
        portEXIT_CRITICAL(&g_lock);
        if (complete) return result;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}

void wifi_remote_service_stop()
{
    portENTER_CRITICAL(&g_lock);
    const bool running = g_running;
    g_stop_requested = true;
    portEXIT_CRITICAL(&g_lock);
    if (!running) return;

    const TickType_t started = xTaskGetTickCount();
    while (static_cast<TickType_t>(xTaskGetTickCount() - started) < kTaskStopTimeout) {
        portENTER_CRITICAL(&g_lock);
        const bool stopped = !g_running;
        portEXIT_CRITICAL(&g_lock);
        if (stopped) return;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "Wi-Fi遥控停止仍在后台收尾");
}

bool wifi_remote_service_is_running()
{
    portENTER_CRITICAL(&g_lock);
    const bool running = g_running;
    portEXIT_CRITICAL(&g_lock);
    return running;
}
