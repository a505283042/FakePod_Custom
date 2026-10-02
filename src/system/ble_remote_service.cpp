#include "ble_remote_service.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_service.h"
#include "battery_service.h"
#include "media_catalog_v2.h"
#include "player_control.h"
#include "wifi_service.h"
#include "app/video_app.h"
#include "app/visual_music_app.h"

#if defined(CONFIG_BT_NIMBLE_ENABLED) && CONFIG_BT_NIMBLE_ENABLED && \
    defined(CONFIG_BT_NIMBLE_ROLE_PERIPHERAL) && CONFIG_BT_NIMBLE_ROLE_PERIPHERAL && \
    defined(CONFIG_BT_NIMBLE_ROLE_BROADCASTER) && CONFIG_BT_NIMBLE_ROLE_BROADCASTER && \
    defined(CONFIG_BT_NIMBLE_GATT_SERVER) && CONFIG_BT_NIMBLE_GATT_SERVER && \
    defined(CONFIG_BT_NIMBLE_GAP_SERVICE) && CONFIG_BT_NIMBLE_GAP_SERVICE
#define FAKEPOD_BLE_FOUNDATION_ENABLED 1
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
extern "C" void ble_store_config_init(void);
#else
#define FAKEPOD_BLE_FOUNDATION_ENABLED 0
#endif

static const char *TAG = "BLE服务";

namespace {

static constexpr const char *kDeviceName = "FakePod";
static constexpr TickType_t kRetryDelay = pdMS_TO_TICKS(2000);
static constexpr TickType_t kStartTimeout = pdMS_TO_TICKS(5000);
static constexpr uint32_t kTransitionTaskStackBytes = 4096U;
static constexpr UBaseType_t kTransitionTaskPriority = 2U;
static constexpr BaseType_t kTransitionTaskCore = 1;

static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static bool g_ready = false;
static bool g_desired_enabled = false;
static bool g_stack_initialized = false;
static bool g_connected = false;
static bool g_transition_running = false;
static BleRemoteState g_state = BleRemoteState::Disabled;
static esp_err_t g_last_error = ESP_OK;
static TickType_t g_retry_due_tick = 0;
static TickType_t g_state_since_tick = 0;

#if FAKEPOD_BLE_FOUNDATION_ENABLED
static uint8_t g_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// R46.0.12：保留手机写命令，并增加 FakePod -> 手机状态 Notify。
// Command/Event UUID 固定；Event 使用 20B Status + <=20B Metadata Chunk，默认 MTU=23 也可工作。
static constexpr uint16_t kMediaPlayerAppearance = 0x0280U;
static constexpr uint8_t kPhoneEventProtocolVersion = 1U;
static constexpr size_t kStatusPacketSize = 20U;
static constexpr size_t kMetadataChunkDataBytes = 14U;
static constexpr size_t kMetadataMaxBytes = 196U;
static constexpr TickType_t kStatusResyncInterval = pdMS_TO_TICKS(15000);
static constexpr TickType_t kStatusEventPollInterval = pdMS_TO_TICKS(250);
// R46.0.52：Notify无确认；每轮元数据完整发送后延迟重放一次，补首连窗口的丢包。
static constexpr TickType_t kMetadataReplayDelay = pdMS_TO_TICKS(750);
// R46.0.55：复用现有加密 Command Characteristic 分片下发 Wi-Fi 凭据，避免新增GATT句柄触发Android缓存。
static constexpr uint8_t kWifiProvisionCommand = 0x20U;
static constexpr size_t kWifiProvisionMaxBytes = 96U; // SSID<=32 + NUL + password<=63
static const ble_uuid128_t kControlServiceUuid = BLE_UUID128_INIT(
    0x64, 0x6F, 0x50, 0x46, 0x6D, 0x0E, 0x3A, 0x8F,
    0x68, 0x4B, 0x4F, 0x2B, 0x10, 0x9C, 0x7A, 0x7D);
static const ble_uuid128_t kControlCommandUuid = BLE_UUID128_INIT(
    0x64, 0x6F, 0x50, 0x46, 0x6D, 0x0E, 0x3A, 0x8F,
    0x68, 0x4B, 0x4F, 0x2B, 0x11, 0x9C, 0x7A, 0x7D);
static const ble_uuid128_t kControlEventUuid = BLE_UUID128_INIT(
    0x64, 0x6F, 0x50, 0x46, 0x6D, 0x0E, 0x3A, 0x8F,
    0x68, 0x4B, 0x4F, 0x2B, 0x12, 0x9C, 0x7A, 0x7D);

static uint16_t g_control_event_val_handle = 0U;
static bool g_control_event_notify_enabled = false;
static bool g_force_status_notify = false;
static bool g_force_metadata_refresh = false;
static TickType_t g_status_notify_due_tick = 0;
static TickType_t g_status_event_poll_due_tick = 0;
static uint8_t g_last_status_packet[kStatusPacketSize] = {};
static uint32_t g_last_status_playback_revision = 0U;
static uint32_t g_last_status_seek_revision = 0U;

enum class PhoneMediaContext : uint8_t
{
    Music = 0U,
    Video,
    Nsf,
};

static bool g_metadata_source_initialized = false;
static PhoneMediaContext g_metadata_context = PhoneMediaContext::Music;
static uint32_t g_metadata_track_index = UINT32_MAX;
static uint32_t g_metadata_catalog_generation = 0U;
static uint32_t g_metadata_source_revision = 0U;
static uint16_t g_metadata_sequence = 0U;
static char g_metadata_payload[kMetadataMaxBytes + 1U] = {};
static size_t g_metadata_size = 0U;
static uint8_t g_metadata_chunk_index = 0U;
static uint8_t g_metadata_chunk_count = 0U;
static uint8_t g_metadata_send_pass = 0U; // 0=首发，1=可靠性补发
static TickType_t g_metadata_replay_due_tick = 0;

static bool g_wifi_provision_active = false;
static uint8_t g_wifi_provision_transaction = 0U;
static uint8_t g_wifi_provision_total = 0U;
static uint8_t g_wifi_provision_next_offset = 0U;
static uint8_t g_wifi_provision_payload[kWifiProvisionMaxBytes] = {};

static struct ble_gatt_chr_def g_control_chrs[3] = {};
static struct ble_gatt_svc_def g_control_svcs[2] = {};
#endif

static bool tick_due(TickType_t now, TickType_t due)
{
    return static_cast<int32_t>(now - due) >= 0;
}

static void set_state_locked(BleRemoteState state, esp_err_t error = ESP_OK)
{
    g_state = state;
    g_last_error = error;
    g_state_since_tick = xTaskGetTickCount();
}

static bool desired_enabled()
{
    portENTER_CRITICAL(&g_lock);
    const bool enabled = g_desired_enabled;
    portEXIT_CRITICAL(&g_lock);
    return enabled;
}

#if FAKEPOD_BLE_FOUNDATION_ENABLED

static const char *control_command_name(uint8_t command)
{
    switch (command) {
        case 0x01U: return "播放/暂停";
        case 0x02U: return "下一曲";
        case 0x03U: return "上一曲";
        case 0x04U: return "音量+";
        case 0x05U: return "音量-";
        case 0x06U: return "音量设置";
        default: return "未知";
    }
}

static bool execute_control_command(uint8_t command, uint8_t value = 0U)
{
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
        case 0x01U: return player_control_toggle_play_pause();
        case 0x02U: return player_control_next();
        case 0x03U: return player_control_previous();
        case 0x04U: return player_control_volume_up(1U);
        case 0x05U: return player_control_volume_down(1U);
        case 0x06U: return value <= 100U && player_control_set_volume(value);
        default: return false;
    }
}

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

static size_t trim_truncated_utf8_tail(char *text, size_t size)
{
    // 只有 snprintf 真的发生截断时才调用；宁可丢掉最后一个完整多字节字符，
    // 也不把半个 UTF-8 字符发给手机。
    while (size > 0U && (static_cast<uint8_t>(text[size - 1U]) & 0xC0U) == 0x80U) {
        --size;
    }
    if (size > 0U && (static_cast<uint8_t>(text[size - 1U]) & 0x80U) != 0U) {
        --size;
    }
    text[size] = '\0';
    return size;
}

static void queue_metadata_text(
    const char *title,
    const char *artist,
    uint32_t track_index,
    uint32_t catalog_generation,
    PhoneMediaContext context,
    uint32_t source_revision)
{
    const int written = snprintf(
        g_metadata_payload,
        sizeof(g_metadata_payload),
        "%s\n%s",
        title != nullptr ? title : "",
        artist != nullptr ? artist : "");

    if (written < 0) {
        g_metadata_payload[0] = '\0';
        g_metadata_size = 0U;
    } else if (static_cast<size_t>(written) >= sizeof(g_metadata_payload)) {
        g_metadata_size = trim_truncated_utf8_tail(g_metadata_payload, kMetadataMaxBytes);
    } else {
        g_metadata_size = static_cast<size_t>(written);
    }

    ++g_metadata_sequence;
    g_metadata_track_index = track_index;
    g_metadata_catalog_generation = catalog_generation;
    g_metadata_context = context;
    g_metadata_source_revision = source_revision;
    g_metadata_source_initialized = true;
    g_metadata_chunk_index = 0U;
    g_metadata_chunk_count = static_cast<uint8_t>(
        (g_metadata_size + kMetadataChunkDataBytes - 1U) / kMetadataChunkDataBytes);
    if (g_metadata_chunk_count == 0U) g_metadata_chunk_count = 1U;
    g_metadata_send_pass = 0U;
    g_metadata_replay_due_tick = 0;
}

static void queue_metadata(uint32_t track_index, uint32_t catalog_generation)
{
    const char *title = "";
    const char *artist = "";
    MediaTrackViewV2 view = {};
    if (track_index != UINT32_MAX && media_catalog_v2_get_track_view(track_index, &view)) {
        if (view.title != nullptr) title = view.title;
        if (view.artist != nullptr) artist = view.artist;
        catalog_generation = view.generation;
    }
    queue_metadata_text(
        title, artist, track_index, catalog_generation, PhoneMediaContext::Music, 0U);
}

static void ensure_metadata_for_track(uint32_t track_index)
{
    uint32_t generation = 0U;
    if (track_index != UINT32_MAX) {
        MediaTrackViewV2 view = {};
        if (media_catalog_v2_get_track_view(track_index, &view)) generation = view.generation;
    }

    if (!g_metadata_source_initialized || g_metadata_context != PhoneMediaContext::Music ||
        track_index != g_metadata_track_index ||
        generation != g_metadata_catalog_generation) {
        queue_metadata(track_index, generation);
    }
}

static void ensure_metadata_for_video(const VideoRemoteSnapshot &video)
{
    if (!g_metadata_source_initialized || g_metadata_context != PhoneMediaContext::Video ||
        video.item_index != g_metadata_track_index ||
        video.metadata_revision != g_metadata_source_revision) {
        queue_metadata_text(
            video.title, "视频", video.item_index, 0U,
            PhoneMediaContext::Video, video.metadata_revision);
    }
}

static void ensure_metadata_for_nsf(const NsfRemoteSnapshot &nsf)
{
    if (!g_metadata_source_initialized || g_metadata_context != PhoneMediaContext::Nsf ||
        nsf.track_index != g_metadata_track_index ||
        nsf.metadata_revision != g_metadata_source_revision) {
        queue_metadata_text(
            nsf.title, nsf.artist, nsf.track_index, 0U,
            PhoneMediaContext::Nsf, nsf.metadata_revision);
    }
}

static void build_status_packet(
    uint8_t *packet,
    uint32_t *out_playback_revision = nullptr,
    uint32_t *out_seek_revision = nullptr)
{
    memset(packet, 0, kStatusPacketSize);
    packet[0] = 0x01U; // Status packet
    packet[1] = kPhoneEventProtocolVersion;

    AudioStateSnapshot audio = {};
    const bool audio_valid = audio_service_get_snapshot(&audio) && audio.ready;
    NsfRemoteSnapshot nsf = {};
    const bool nsf_valid = visual_music_app_get_remote_snapshot(&nsf) && nsf.active;
    VideoRemoteSnapshot video = {};
    const bool video_valid = !nsf_valid && video_app_get_remote_snapshot(&video) && video.active;
    const uint32_t track_index = nsf_valid
        ? nsf.track_index
        : (video_valid ? video.item_index : (audio_valid ? audio.track_index : UINT32_MAX));
    if (out_playback_revision != nullptr) {
        *out_playback_revision = nsf_valid
            ? nsf.playback_revision
            : (video_valid ? video.playback_revision : (audio_valid ? audio.playback_revision : 0U));
    }
    if (out_seek_revision != nullptr) {
        *out_seek_revision = (nsf_valid || video_valid) ? 0U : (audio_valid ? audio.seek_revision : 0U);
    }
    if (nsf_valid) ensure_metadata_for_nsf(nsf);
    else if (video_valid) ensure_metadata_for_video(video);
    else ensure_metadata_for_track(track_index);

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
    if (track_index != UINT32_MAX) flags |= 0x04U;
    if (video_valid) flags |= 0x08U; // media context = VIDEO
    if (nsf_valid) flags |= 0x10U;   // R46.0.50: media context = NSF
    packet[5] = flags;

    write_u32_le(&packet[6], track_index);
    write_u32_le(&packet[10], nsf_valid
        ? nsf.position_ms
        : (video_valid
            ? video.position_ms
            : (audio_valid ? clamp_u64_to_u32(audio.position_ms) : 0U)));

    uint64_t duration_ms = nsf_valid ? nsf.duration_ms : (video_valid ? video.duration_ms : 0ULL);
    if (!nsf_valid && !video_valid && audio_valid &&
        audio.sample_rate_hz > 0U && audio.total_frames > 0ULL) {
        duration_ms = (audio.total_frames * 1000ULL) / audio.sample_rate_hz;
    }
    write_u32_le(&packet[14], clamp_u64_to_u32(duration_ms));
    write_u16_le(&packet[18], g_metadata_sequence);
}

static int status_event_access(
    uint16_t,
    uint16_t,
    struct ble_gatt_access_ctxt *ctxt,
    void *)
{
    if (ctxt == nullptr || ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR || ctxt->om == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t packet[kStatusPacketSize] = {};
    portENTER_CRITICAL(&g_lock);
    memcpy(packet, g_last_status_packet, sizeof(packet));
    portEXIT_CRITICAL(&g_lock);
    return os_mbuf_append(ctxt->om, packet, sizeof(packet)) == 0
        ? 0
        : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static bool connection_is_encrypted(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc = {};
    return ble_gap_conn_find(conn_handle, &desc) == 0 && desc.sec_state.encrypted;
}

static int notify_phone_event(uint16_t conn_handle, const void *data, size_t size)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, size);
    if (om == nullptr) return BLE_HS_ENOMEM;
    return ble_gatts_notify_custom(conn_handle, g_control_event_val_handle, om);
}

static bool status_semantic_changed(const uint8_t *packet)
{
    // position_ms(10..13) 由手机本地单调时钟推进，不参与事件变化判断。
    // battery_percent(4) 与 battery_valid 标志位(5.bit1) 只随 15 秒校时或其它事件顺带同步，
    // 避免电量 ADC 在百分比边界附近抖动时单独触发 BLE Notify。
    const uint8_t flags_changed = static_cast<uint8_t>(
        (packet[5] ^ g_last_status_packet[5]) & static_cast<uint8_t>(~0x02U));
    return packet[2] != g_last_status_packet[2] ||
        packet[3] != g_last_status_packet[3] ||
        flags_changed != 0U ||
        memcmp(&packet[6], &g_last_status_packet[6], 4U) != 0 ||
        memcmp(&packet[14], &g_last_status_packet[14], 6U) != 0;
}

static bool send_next_metadata_chunk(uint16_t conn_handle)
{
    if (g_metadata_chunk_index >= g_metadata_chunk_count) return false;

    uint8_t packet[6U + kMetadataChunkDataBytes] = {};
    packet[0] = 0x02U; // Metadata chunk
    packet[1] = kPhoneEventProtocolVersion;
    write_u16_le(&packet[2], g_metadata_sequence);
    packet[4] = g_metadata_chunk_index;
    packet[5] = g_metadata_chunk_count;

    const size_t offset = static_cast<size_t>(g_metadata_chunk_index) * kMetadataChunkDataBytes;
    const size_t remaining = offset < g_metadata_size ? g_metadata_size - offset : 0U;
    const size_t chunk_size = remaining > kMetadataChunkDataBytes
        ? kMetadataChunkDataBytes
        : remaining;
    if (chunk_size > 0U) memcpy(&packet[6], &g_metadata_payload[offset], chunk_size);

    const int rc = notify_phone_event(conn_handle, packet, 6U + chunk_size);
    if (rc != 0) return true;

    ++g_metadata_chunk_index;
    if (g_metadata_chunk_index >= g_metadata_chunk_count && g_metadata_send_pass == 0U) {
        g_metadata_replay_due_tick = xTaskGetTickCount() + kMetadataReplayDelay;
    }
    return g_metadata_chunk_index < g_metadata_chunk_count;
}

static void update_phone_state_notify(TickType_t now)
{
    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    bool notify_enabled = false;
    bool force_status = false;
    bool force_metadata_refresh = false;
    TickType_t resync_due_tick = 0;
    TickType_t poll_due_tick = 0;

    portENTER_CRITICAL(&g_lock);
    conn_handle = g_conn_handle;
    notify_enabled = g_control_event_notify_enabled;
    force_status = g_force_status_notify;
    force_metadata_refresh = g_force_metadata_refresh;
    resync_due_tick = g_status_notify_due_tick;
    poll_due_tick = g_status_event_poll_due_tick;
    portEXIT_CRITICAL(&g_lock);

    if (!notify_enabled || conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        g_control_event_val_handle == 0U || !connection_is_encrypted(conn_handle)) {
        return;
    }

    if (force_metadata_refresh) {
        portENTER_CRITICAL(&g_lock);
        g_force_metadata_refresh = false;
        portEXIT_CRITICAL(&g_lock);
        // Metadata 缓冲只由 system loop 维护，避免 GAP HostTask 与 system loop 并发改写。
        g_metadata_source_initialized = false;
        g_metadata_chunk_index = 0U;
        g_metadata_chunk_count = 0U;
        g_metadata_send_pass = 0U;
        g_metadata_replay_due_tick = 0;
    }

    if (g_metadata_send_pass == 0U && g_metadata_chunk_count > 0U &&
        g_metadata_chunk_index >= g_metadata_chunk_count &&
        g_metadata_replay_due_tick != 0 && tick_due(now, g_metadata_replay_due_tick)) {
        g_metadata_send_pass = 1U;
        g_metadata_chunk_index = 0U;
        g_metadata_replay_due_tick = 0;
        ESP_LOGI(TAG, "BLE元数据可靠性补发：seq=%u chunks=%u",
            static_cast<unsigned>(g_metadata_sequence),
            static_cast<unsigned>(g_metadata_chunk_count));
    }

    const bool resync_due = resync_due_tick == 0 || tick_due(now, resync_due_tick);
    const bool poll_due = poll_due_tick == 0 || tick_due(now, poll_due_tick);
    if (force_status || resync_due || poll_due) {
        uint8_t packet[kStatusPacketSize] = {};
        uint32_t playback_revision = 0U;
        uint32_t seek_revision = 0U;
        build_status_packet(packet, &playback_revision, &seek_revision);
        const bool semantic_changed = status_semantic_changed(packet);
        const bool timeline_changed =
            playback_revision != g_last_status_playback_revision ||
            seek_revision != g_last_status_seek_revision;

        portENTER_CRITICAL(&g_lock);
        g_status_event_poll_due_tick = now + kStatusEventPollInterval;
        portEXIT_CRITICAL(&g_lock);

        // Stable Playing 时 position_ms 不再每 500ms 通过 BLE 推送；手机本地自走。
        // Track/Seek revision 即使跨过短暂 Seeking 状态，也会触发一次准确时间同步。
        if (force_status || resync_due || semantic_changed || timeline_changed) {
            const int rc = notify_phone_event(conn_handle, packet, sizeof(packet));
            if (rc == 0) {
                portENTER_CRITICAL(&g_lock);
                memcpy(g_last_status_packet, packet, sizeof(packet));
                g_last_status_playback_revision = playback_revision;
                g_last_status_seek_revision = seek_revision;
                g_force_status_notify = false;
                g_status_notify_due_tick = now + kStatusResyncInterval;
                portEXIT_CRITICAL(&g_lock);
            }
            return;
        }
    }

    (void)send_next_metadata_chunk(conn_handle);
}

static void reset_wifi_provision_assembly()
{
    g_wifi_provision_active = false;
    g_wifi_provision_transaction = 0U;
    g_wifi_provision_total = 0U;
    g_wifi_provision_next_offset = 0U;
    memset(g_wifi_provision_payload, 0, sizeof(g_wifi_provision_payload));
}

static int handle_wifi_provision_fragment(const uint8_t *payload, size_t payload_len)
{
    // Packet: 0x20, transaction, total_bytes, offset, <=16B payload.
    if (payload == nullptr || payload_len < 5U || payload_len > 20U) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    const uint8_t transaction = payload[1];
    const uint8_t total = payload[2];
    const uint8_t offset = payload[3];
    const size_t chunk_size = payload_len - 4U;
    if (total < 2U || total > kWifiProvisionMaxBytes ||
        offset >= total || static_cast<size_t>(offset) + chunk_size > total) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (offset == 0U) {
        reset_wifi_provision_assembly();
        g_wifi_provision_active = true;
        g_wifi_provision_transaction = transaction;
        g_wifi_provision_total = total;
    } else if (!g_wifi_provision_active ||
        transaction != g_wifi_provision_transaction ||
        total != g_wifi_provision_total ||
        offset != g_wifi_provision_next_offset) {
        ESP_LOGW(TAG, "BLE Wi-Fi配网分片乱序：tx=%u total=%u offset=%u expected=%u",
            static_cast<unsigned>(transaction),
            static_cast<unsigned>(total),
            static_cast<unsigned>(offset),
            static_cast<unsigned>(g_wifi_provision_next_offset));
        reset_wifi_provision_assembly();
        return BLE_ATT_ERR_UNLIKELY;
    }

    memcpy(&g_wifi_provision_payload[offset], &payload[4], chunk_size);
    g_wifi_provision_next_offset = static_cast<uint8_t>(offset + chunk_size);
    if (g_wifi_provision_next_offset < g_wifi_provision_total) return 0;

    size_t separator = 0U;
    while (separator < g_wifi_provision_total && g_wifi_provision_payload[separator] != 0U) {
        ++separator;
    }
    if (separator == 0U || separator > 32U || separator >= g_wifi_provision_total) {
        reset_wifi_provision_assembly();
        return BLE_ATT_ERR_UNLIKELY;
    }

    const size_t password_len = static_cast<size_t>(g_wifi_provision_total) - separator - 1U;
    if (password_len > 63U) {
        reset_wifi_provision_assembly();
        return BLE_ATT_ERR_UNLIKELY;
    }

    char ssid[33] = {};
    char password[64] = {};
    memcpy(ssid, g_wifi_provision_payload, separator);
    if (password_len > 0U) {
        memcpy(password, &g_wifi_provision_payload[separator + 1U], password_len);
    }

    const esp_err_t ret = wifi_service_submit_credentials(ssid, password);
    reset_wifi_provision_assembly();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE Wi-Fi配网提交失败：ssid=%s ret=%s", ssid, esp_err_to_name(ret));
        return BLE_ATT_ERR_UNLIKELY;
    }

    ESP_LOGI(TAG, "BLE Wi-Fi配网接收完成：ssid=%s password=%uB；即将关闭BLE并连接Wi-Fi",
        ssid,
        static_cast<unsigned>(password_len));
    return 0;
}

static int control_command_access(
    uint16_t conn_handle,
    uint16_t,
    struct ble_gatt_access_ctxt *ctxt,
    void *)
{
    if (ctxt == nullptr || ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || ctxt->om == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const size_t payload_len = OS_MBUF_PKTLEN(ctxt->om);
    if (payload_len < 1U || payload_len > 20U) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t payload[20] = {};
    if (os_mbuf_copydata(ctxt->om, 0, payload_len, payload) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint8_t command = payload[0];
    if (command == kWifiProvisionCommand) {
        return handle_wifi_provision_fragment(payload, payload_len);
    }

    if (payload_len != 1U && payload_len != 2U) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    const bool absolute_volume = command == 0x06U;
    if (absolute_volume) {
        if (payload_len != 2U) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (payload[1] > 100U) {
            ESP_LOGW(TAG, "BLE手机控制音量越界：%u%%", static_cast<unsigned>(payload[1]));
            return BLE_ATT_ERR_UNLIKELY;
        }
    } else if (payload_len != 1U || command < 0x01U || command > 0x05U) {
        ESP_LOGW(TAG, "BLE手机控制未知命令：0x%02X len=%u",
            static_cast<unsigned>(command),
            static_cast<unsigned>(payload_len));
        return BLE_ATT_ERR_UNLIKELY;
    }

    const bool ok = execute_control_command(command, payload[1]);
    if (!ok) {
        ESP_LOGW(TAG, "BLE手机控制执行失败：handle=%u command=0x%02X %s",
            static_cast<unsigned>(conn_handle),
            static_cast<unsigned>(command),
            control_command_name(command));
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (absolute_volume) {
        ESP_LOGI(TAG, "BLE手机控制：handle=%u command=0x06 音量=%u%%",
            static_cast<unsigned>(conn_handle),
            static_cast<unsigned>(payload[1]));
    } else {
        const char *context = "MUSIC";
        if (command <= 0x03U) {
            if (visual_music_app_remote_control_active()) context = "NSF";
            else if (video_app_remote_control_active()) context = "VIDEO";
        }
        ESP_LOGI(TAG, "BLE手机控制：handle=%u command=0x%02X %s context=%s",
            static_cast<unsigned>(conn_handle),
            static_cast<unsigned>(command),
            control_command_name(command),
            context);
    }
    return 0;
}

static void init_control_gatt_defs()
{
    memset(g_control_chrs, 0, sizeof(g_control_chrs));
    memset(g_control_svcs, 0, sizeof(g_control_svcs));

    g_control_chrs[0].uuid = &kControlCommandUuid.u;
    g_control_chrs[0].access_cb = control_command_access;
    g_control_chrs[0].flags = BLE_GATT_CHR_F_WRITE |
        BLE_GATT_CHR_F_WRITE_NO_RSP |
        BLE_GATT_CHR_F_WRITE_ENC;

    g_control_chrs[1].uuid = &kControlEventUuid.u;
    g_control_chrs[1].access_cb = status_event_access;
    g_control_chrs[1].flags = BLE_GATT_CHR_F_READ |
        BLE_GATT_CHR_F_READ_ENC |
        BLE_GATT_CHR_F_NOTIFY;
    g_control_chrs[1].val_handle = &g_control_event_val_handle;

    g_control_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_control_svcs[0].uuid = &kControlServiceUuid.u;
    g_control_svcs[0].characteristics = g_control_chrs;
}

static int register_control_service()
{
    init_control_gatt_defs();
    int rc = ble_gatts_count_cfg(g_control_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(g_control_svcs);
    return rc;
}

static bool should_advertise()
{
    portENTER_CRITICAL(&g_lock);
    const bool allowed = g_desired_enabled &&
        !g_connected &&
        g_state != BleRemoteState::Stopping;
    portEXIT_CRITICAL(&g_lock);
    return allowed;
}


static int start_advertising();

static void retry_advertising_after_failure(const char *reason, int rc)
{
    portENTER_CRITICAL(&g_lock);
    if (g_desired_enabled && g_state != BleRemoteState::Stopping) {
        set_state_locked(BleRemoteState::RetryWait, ESP_FAIL);
        g_retry_due_tick = xTaskGetTickCount() + kRetryDelay;
    }
    portEXIT_CRITICAL(&g_lock);
    ESP_LOGW(TAG, "BLE广播恢复失败：source=%s rc=%d；2秒后完整重试", reason, rc);
}

static void restart_advertising_or_retry(const char *reason)
{
    if (!should_advertise()) return;
    const int rc = start_advertising();
    if (rc != 0) retry_advertising_after_failure(reason, rc);
}


static int gap_event_cb(struct ble_gap_event *event, void *)
{
    if (event == nullptr) return 0;

    switch (event->type) {

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                portENTER_CRITICAL(&g_lock);
                g_connected = true;
                g_conn_handle = event->connect.conn_handle;
                g_control_event_notify_enabled = false;
                g_force_status_notify = false;
                g_force_metadata_refresh = false;
                g_status_notify_due_tick = 0;
                g_status_event_poll_due_tick = 0;
                set_state_locked(BleRemoteState::Connected);
                portEXIT_CRITICAL(&g_lock);
                ESP_LOGI(TAG, "手机已连接：handle=%u", static_cast<unsigned>(event->connect.conn_handle));
            } else {
                ESP_LOGW(TAG, "BLE连接尝试失败：status=%d；恢复广播", event->connect.status);
                restart_advertising_or_retry("connect_failed");
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT: {
            portENTER_CRITICAL(&g_lock);
            g_connected = false;
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_control_event_notify_enabled = false;
            g_force_status_notify = false;
            g_force_metadata_refresh = false;
            g_status_notify_due_tick = 0;
            g_status_event_poll_due_tick = 0;
            const bool resume_broadcast = g_desired_enabled &&
                g_state != BleRemoteState::Stopping;
            if (g_desired_enabled && g_state != BleRemoteState::Stopping) {
                // 广播成功前不能提前标记 Advertising，否则一次 adv_start 失败后
                // update() 会误以为广播仍在运行，造成断线后永久不再重试。
                set_state_locked(BleRemoteState::Starting);
            }
            portEXIT_CRITICAL(&g_lock);
            ESP_LOGI(TAG, "BLE连接已断开：reason=%d", event->disconnect.reason);
            if (resume_broadcast) restart_advertising_or_retry("disconnect");
            return 0;
        }

        case BLE_GAP_EVENT_ADV_COMPLETE:
            restart_advertising_or_retry("adv_complete");
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE:
            // 成功加密属于正常路径，不再刷诊断日志；失败仍保留告警。
            if (event->enc_change.status != 0) {
                ESP_LOGW(TAG, "BLE加密失败：handle=%u status=%d",
                    static_cast<unsigned>(event->enc_change.conn_handle),
                    event->enc_change.status);
            }
            return 0;

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            struct ble_gap_conn_desc desc = {};
            const int find_rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
            if (find_rc != 0) {
                ESP_LOGW(TAG, "BLE重复配对读取连接失败：handle=%u rc=%d",
                    static_cast<unsigned>(event->repeat_pairing.conn_handle),
                    find_rc);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }

            // 手机删除配对后会丢失自己的 LTK；设备端仍保留旧 Bond 时，NimBLE 会报告重复配对。
            // 只删除当前 peer 的旧 Bond，并让同一条连接继续执行新的 SMP 配对。
            const int delete_rc = ble_store_util_delete_peer(&desc.peer_id_addr);
            if (delete_rc != 0) {
                ESP_LOGW(TAG, "BLE重复配对删除旧Bond失败：handle=%u rc=%d",
                    static_cast<unsigned>(event->repeat_pairing.conn_handle),
                    delete_rc);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }

            ESP_LOGI(TAG, "BLE重复配对：旧Bond已删除，继续重新配对：handle=%u",
                static_cast<unsigned>(event->repeat_pairing.conn_handle));
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == g_control_event_val_handle) {
                portENTER_CRITICAL(&g_lock);
                g_control_event_notify_enabled = event->subscribe.cur_notify != 0U;
                g_force_status_notify = g_control_event_notify_enabled;
                g_force_metadata_refresh = g_control_event_notify_enabled;
                g_status_notify_due_tick = 0;
                g_status_event_poll_due_tick = 0;
                portEXIT_CRITICAL(&g_lock);
                ESP_LOGI(TAG, "BLE状态订阅：notify=%u",
                    static_cast<unsigned>(event->subscribe.cur_notify));
            }
            return 0;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "BLE MTU更新：handle=%u mtu=%u",
                static_cast<unsigned>(event->mtu.conn_handle),
                static_cast<unsigned>(event->mtu.value));
            return 0;

        default:
            return 0;
    }
}


static int start_advertising()
{
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<uint8_t *>(const_cast<char *>(kDeviceName));
    fields.name_len = strlen(kDeviceName);
    fields.name_is_complete = 1;
    fields.uuids128 = const_cast<ble_uuid128_t *>(&kControlServiceUuid);
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // R46.0 只验证控制链路，不追求快速发现；500ms 广播降低后台负载和功耗。
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(500);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(510);

    rc = ble_gap_adv_start(
        g_own_addr_type,
        nullptr,
        BLE_HS_FOREVER,
        &params,
        gap_event_cb,
        nullptr);
    if (rc == BLE_HS_EALREADY) rc = 0;
    if (rc == 0) {
        portENTER_CRITICAL(&g_lock);
        if (!g_connected && g_desired_enabled && g_state != BleRemoteState::Stopping) {
            set_state_locked(BleRemoteState::Advertising);
            g_retry_due_tick = 0;
        }
        portEXIT_CRITICAL(&g_lock);
    }
    return rc;
}

static void host_reset_cb(int reason)
{
    ESP_LOGW(TAG, "NimBLE Host reset：reason=%d", reason);
    portENTER_CRITICAL(&g_lock);
    if (g_desired_enabled && g_state != BleRemoteState::Stopping) {
        set_state_locked(BleRemoteState::RetryWait, ESP_FAIL);
        g_retry_due_tick = xTaskGetTickCount() + kRetryDelay;
    }
    portEXIT_CRITICAL(&g_lock);
}

static void log_bond_store_status()
{
#if defined(CONFIG_BT_NIMBLE_NVS_PERSIST) && CONFIG_BT_NIMBLE_NVS_PERSIST
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS] = {};
    int peer_count = 0;
    const int rc = ble_store_util_bonded_peers(
        peers,
        &peer_count,
        CONFIG_BT_NIMBLE_MAX_BONDS);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE Bond存储：NVS=ON peers=%d", peer_count);
    } else {
        ESP_LOGW(TAG, "BLE Bond存储读取失败：NVS=ON rc=%d", rc);
    }
#else
    ESP_LOGW(TAG, "BLE Bond存储：NVS=OFF");
#endif
}

static void host_sync_cb()
{
    log_bond_store_status();

    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &g_own_addr_type);

    if (rc == 0) rc = start_advertising();

    if (rc != 0) {
        ESP_LOGE(TAG, "BLE广播启动失败：rc=%d", rc);
        portENTER_CRITICAL(&g_lock);
        if (g_desired_enabled) {
            set_state_locked(BleRemoteState::RetryWait, ESP_FAIL);
            g_retry_due_tick = xTaskGetTickCount() + kRetryDelay;
        }
        portEXIT_CRITICAL(&g_lock);
        return;
    }

    ESP_LOGI(TAG, "BLE广播已启动：name=%s interval≈500ms", kDeviceName);
}

static void host_task(void *)
{
    ESP_LOGI(TAG, "NimBLE HostTask启动：Core=%d", xPortGetCoreID());
    nimble_port_run();
    // 官方 FreeRTOS port 要求 HostTask 在 nimble_port_run() 返回后完成自身反初始化。
    nimble_port_freertos_deinit();
}

static esp_err_t stop_stack()
{
    portENTER_CRITICAL(&g_lock);
    const bool initialized = g_stack_initialized;
    portEXIT_CRITICAL(&g_lock);
    if (!initialized) return ESP_OK;

    // nimble_port_stop() 自己会停止 GAP procedure 并终止现有连接。
    // 这里不能先手动 terminate，否则 stop 内部再次终止会命中 BLE_HS_EALREADY。
    const int stop_rc = nimble_port_stop();
    if (stop_rc != 0) {
        ESP_LOGW(TAG, "NimBLE停止失败：rc=%d", stop_rc);
        return ESP_FAIL;
    }

    // ESP-IDF 5.x 官方流程：stop 后 deinit；不额外调用不可逆的 bt_mem_release。
    nimble_port_deinit();

    portENTER_CRITICAL(&g_lock);
    g_stack_initialized = false;
    g_connected = false;
    g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    g_control_event_notify_enabled = false;
    g_force_status_notify = false;
    g_force_metadata_refresh = false;
    g_status_notify_due_tick = 0;
    g_status_event_poll_due_tick = 0;
    portEXIT_CRITICAL(&g_lock);
    reset_wifi_provision_assembly();
    return ESP_OK;
}

static esp_err_t start_stack()
{
#if defined(CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT) && CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT
    ESP_LOGW(TAG, "BLE GAP重连策略：NimBLE内建0x3E重试=ON（应为OFF，请清理旧sdkconfig后重编）");
#else
    ESP_LOGI(TAG, "BLE GAP重连策略：NimBLE内建0x3E重试=OFF，广播恢复由FakePod状态机接管");
#endif
#if defined(CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL) && CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL
    const char *nimble_alloc_mode = "PSRAM";
#else
    const char *nimble_alloc_mode = "INTERNAL/DEFAULT";
    ESP_LOGW(TAG, "BLE RAM策略：NimBLE Host未使用PSRAM（应为PSRAM，请清理旧sdkconfig后重编）");
#endif
    ESP_LOGI(TAG,
        "BLE RAM配置：alloc=%s connections=%d activities=%d host_stack=%d ACL=%dx%d MSYS1=%dx%d MSYS2=%dx%d EVT=%d+%d",
        nimble_alloc_mode,
        CONFIG_BT_NIMBLE_MAX_CONNECTIONS,
        CONFIG_BT_CTRL_BLE_MAX_ACT,
        CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE,
        CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT,
        CONFIG_BT_NIMBLE_TRANSPORT_ACL_SIZE,
        CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT,
        CONFIG_BT_NIMBLE_MSYS_1_BLOCK_SIZE,
        CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT,
        CONFIG_BT_NIMBLE_MSYS_2_BLOCK_SIZE,
        CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT,
        CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT);
    const int init_rc = nimble_port_init();
    if (init_rc != ESP_OK) {
        ESP_LOGW(TAG, "NimBLE初始化失败：rc=%d", init_rc);
        return init_rc == ESP_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    portENTER_CRITICAL(&g_lock);
    g_stack_initialized = true;
    g_connected = false;
    g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    g_control_event_notify_enabled = false;
    g_force_status_notify = false;
    g_force_metadata_refresh = false;
    g_status_notify_due_tick = 0;
    g_status_event_poll_due_tick = 0;
    portEXIT_CRITICAL(&g_lock);

    g_control_event_val_handle = 0U;
    g_metadata_source_initialized = false;
    g_metadata_chunk_index = 0U;
    g_metadata_chunk_count = 0U;
    g_metadata_send_pass = 0U;
    g_metadata_replay_due_tick = 0;
    reset_wifi_provision_assembly();
    memset(g_last_status_packet, 0, sizeof(g_last_status_packet));
    g_last_status_playback_revision = 0U;
    g_last_status_seek_revision = 0U;
    g_last_status_packet[0] = 0x01U;
    g_last_status_packet[1] = kPhoneEventProtocolVersion;
    g_last_status_packet[2] = 0xFFU;
    g_last_status_packet[4] = 0xFFU;
    write_u32_le(&g_last_status_packet[6], UINT32_MAX);

    ble_hs_cfg.reset_cb = host_reset_cb;
    ble_hs_cfg.sync_cb = host_sync_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // R46.0.14.3：显式启用 Bond，并交换加密键/身份键。
    // 仅有 NVS 持久化并不等价于开启 Bond；手机后续重连要靠已保存的 LTK/IRK 恢复加密。
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    const int appearance_rc = ble_svc_gap_device_appearance_set(kMediaPlayerAppearance);
    if (appearance_rc != 0) {
        ESP_LOGW(TAG, "BLE Media Player Appearance设置失败：rc=%d", appearance_rc);
        (void)stop_stack();
        return ESP_FAIL;
    }

    const int control_rc = register_control_service();
    if (control_rc != 0) {
        ESP_LOGW(TAG, "BLE手机控制服务注册失败：rc=%d", control_rc);
        (void)stop_stack();
        return ESP_FAIL;
    }

    const int name_rc = ble_svc_gap_device_name_set(kDeviceName);
    if (name_rc != 0) {
        ESP_LOGW(TAG, "BLE设备名设置失败：rc=%d", name_rc);
        (void)stop_stack();
        return ESP_FAIL;
    }

    // NimBLE 安全存储接入默认 NVS，使 Bond/LTK 在设备重启后仍可恢复。
    ble_store_config_init();

    ESP_LOGI(TAG, "BLE手机控制服务已注册：6媒体命令 + Wi-Fi配网分片 + 状态Notify V1");
    ESP_LOGI(TAG, "BLE安全：Bond=ON SC=ON MITM=OFF KeyDist=ENC|ID");
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

#endif // FAKEPOD_BLE_FOUNDATION_ENABLED

static void transition_task(void *)
{
#if FAKEPOD_BLE_FOUNDATION_ENABLED
    esp_err_t ret = ESP_OK;
    const bool enable = desired_enabled();

    portENTER_CRITICAL(&g_lock);
    const bool stack_initialized = g_stack_initialized;
    portEXIT_CRITICAL(&g_lock);

    if (!enable) {
        ret = stop_stack();
        portENTER_CRITICAL(&g_lock);
        if (ret == ESP_OK) {
            set_state_locked(BleRemoteState::Disabled);
        } else {
            set_state_locked(BleRemoteState::RetryWait, ret);
            g_retry_due_tick = xTaskGetTickCount() + kRetryDelay;
        }
        g_transition_running = false;
        portEXIT_CRITICAL(&g_lock);
        vTaskDelete(nullptr);
        return;
    }

    // RetryWait 可能来自 Host reset/广播失败；先完整收掉旧栈，再重新初始化。
    if (stack_initialized) {
        ret = stop_stack();
    }
    if (ret == ESP_OK && desired_enabled()) {
        portENTER_CRITICAL(&g_lock);
        set_state_locked(BleRemoteState::Starting);
        portEXIT_CRITICAL(&g_lock);
        ret = start_stack();
    }

    portENTER_CRITICAL(&g_lock);
    if (ret != ESP_OK) {
        set_state_locked(BleRemoteState::RetryWait, ret);
        g_retry_due_tick = xTaskGetTickCount() + kRetryDelay;
    } else if (!g_desired_enabled) {
        // 用户在启动过程中又关闭；下一轮 update() 会立刻进入 stop。
        set_state_locked(BleRemoteState::Stopping);
    }
    g_transition_running = false;
    portEXIT_CRITICAL(&g_lock);
#else
    portENTER_CRITICAL(&g_lock);
    set_state_locked(BleRemoteState::Unsupported, ESP_ERR_NOT_SUPPORTED);
    g_transition_running = false;
    portEXIT_CRITICAL(&g_lock);
#endif
    vTaskDelete(nullptr);
}

static bool start_transition_task(BleRemoteState state)
{
    portENTER_CRITICAL(&g_lock);
    if (g_transition_running) {
        portEXIT_CRITICAL(&g_lock);
        return true;
    }
    g_transition_running = true;
    set_state_locked(state);
    portEXIT_CRITICAL(&g_lock);

    const BaseType_t created = xTaskCreatePinnedToCore(
        transition_task,
        "ble_ctl",
        kTransitionTaskStackBytes,
        nullptr,
        kTransitionTaskPriority,
        nullptr,
        kTransitionTaskCore);
    if (created == pdPASS) return true;

    portENTER_CRITICAL(&g_lock);
    g_transition_running = false;
    set_state_locked(BleRemoteState::RetryWait, ESP_ERR_NO_MEM);
    g_retry_due_tick = xTaskGetTickCount() + kRetryDelay;
    portEXIT_CRITICAL(&g_lock);
    ESP_LOGW(TAG, "BLE控制任务创建失败：NO_MEM；2秒后重试");
    return false;
}

} // namespace

esp_err_t ble_remote_service_init()
{
    portENTER_CRITICAL(&g_lock);
    if (g_ready) {
        const BleRemoteState state = g_state;
        portEXIT_CRITICAL(&g_lock);
        return state == BleRemoteState::Unsupported ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
    }
    g_ready = true;
    g_desired_enabled = false;
    g_stack_initialized = false;
    g_connected = false;
    g_transition_running = false;
    g_last_error = ESP_OK;
    g_retry_due_tick = 0;
#if FAKEPOD_BLE_FOUNDATION_ENABLED
    set_state_locked(BleRemoteState::Disabled);
#else
    set_state_locked(BleRemoteState::Unsupported, ESP_ERR_NOT_SUPPORTED);
#endif
#if !FAKEPOD_BLE_FOUNDATION_ENABLED
    const BleRemoteState state = g_state;
#endif
    portEXIT_CRITICAL(&g_lock);

#if FAKEPOD_BLE_FOUNDATION_ENABLED
    ESP_LOGI(TAG, "BLE Foundation就绪：NimBLE Peripheral/GATT Server，Observer/Scan/Central已裁剪，默认关闭");
    return ESP_OK;
#else
    ESP_LOGW(TAG, "BLE Foundation代码已就绪，但 sdkconfig 尚未完整启用 NimBLE Peripheral/Broadcaster/GATT Server/GAP Service");
    return state == BleRemoteState::Unsupported ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
#endif
}

void ble_remote_service_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&g_lock);
    if (!g_ready) {
        portEXIT_CRITICAL(&g_lock);
        return;
    }
    g_desired_enabled = enabled;
    g_retry_due_tick = 0;
    portEXIT_CRITICAL(&g_lock);
    ESP_LOGI(TAG, "BLE用户意图：%s", enabled ? "开启" : "关闭");
}

void ble_remote_service_update()
{
    portENTER_CRITICAL(&g_lock);
    if (!g_ready || g_transition_running) {
        portEXIT_CRITICAL(&g_lock);
        return;
    }
    const bool desired = g_desired_enabled;
    BleRemoteState state = g_state;
    const TickType_t retry_due = g_retry_due_tick;
    const TickType_t state_since = g_state_since_tick;
    portEXIT_CRITICAL(&g_lock);

    if (state == BleRemoteState::Unsupported) return;

    const TickType_t now = xTaskGetTickCount();
    if (desired) {
        if (state == BleRemoteState::Connected) {
#if FAKEPOD_BLE_FOUNDATION_ENABLED
            update_phone_state_notify(now);
#endif
            return;
        }
        if (state == BleRemoteState::Advertising) return;
        if (state == BleRemoteState::Stopping) return;
        if (state == BleRemoteState::Starting && !tick_due(now, state_since + kStartTimeout)) return;
        if (state == BleRemoteState::Starting) {
            ESP_LOGW(TAG, "BLE启动5秒仍未同步，执行完整重启");
            portENTER_CRITICAL(&g_lock);
            set_state_locked(BleRemoteState::RetryWait, ESP_ERR_TIMEOUT);
            g_retry_due_tick = now;
            portEXIT_CRITICAL(&g_lock);
            state = BleRemoteState::RetryWait;
        }
        if (state == BleRemoteState::RetryWait && retry_due != 0 && !tick_due(now, retry_due)) return;
        (void)start_transition_task(BleRemoteState::Starting);
        return;
    }

    if (state == BleRemoteState::Disabled) return;
    if (state == BleRemoteState::RetryWait && retry_due != 0 && !tick_due(now, retry_due)) return;
    (void)start_transition_task(BleRemoteState::Stopping);
}

bool ble_remote_service_get_snapshot(BleRemoteSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    portENTER_CRITICAL(&g_lock);
    out_snapshot->ready = g_ready;
    out_snapshot->desired_enabled = g_desired_enabled;
    out_snapshot->stack_initialized = g_stack_initialized;
    out_snapshot->connected = g_connected;
    out_snapshot->state = g_state;
    out_snapshot->last_error = g_last_error;
    portEXIT_CRITICAL(&g_lock);
    return out_snapshot->ready;
}

const char *ble_remote_service_state_name(BleRemoteState state)
{
    switch (state) {
        case BleRemoteState::Disabled: return "关闭";
        case BleRemoteState::Starting: return "启动中";
        case BleRemoteState::Advertising: return "广播中";
        case BleRemoteState::Connected: return "已连接";
        case BleRemoteState::Stopping: return "关闭中";
        case BleRemoteState::RetryWait: return "故障重试";
        case BleRemoteState::Unsupported: return "未启用NimBLE";
        default: return "未知";
    }
}
