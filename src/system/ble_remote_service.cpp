#include "ble_remote_service.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_service.h"
#include "app/app_manager.h"
#include "battery_service.h"
#include "media_catalog_v2.h"
#include "player_control.h"
#include "remote_music_seek.h"
#include "ui/screens/library_view.h"
#include "ui/screens/player_home.h"
#include "screen_lock_simple.h"
#include "player_state.h"
#include "wifi_service.h"
#include "app/video_app.h"
#include "app/visual_music_app.h"
#include "gamepad_input_service.h"

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
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_hidh.h"
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
static DeviceBluetoothMode g_mode = DeviceBluetoothMode::PhoneRemote;
static DeviceBluetoothMode g_active_mode = DeviceBluetoothMode::Off;
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

// BLE HID Host state. Only one radio connection is allowed by sdkconfig; phone and
// controller modes are mutually exclusive and share the same NimBLE host lifecycle.
static constexpr uint16_t kHidServiceUuid16 = 0x1812U;
static constexpr uint16_t kGamepadAppearance = 0x03C4U;
static constexpr uint16_t kJoystickAppearance = 0x03C3U;
static constexpr uint32_t kControllerScanDurationMs = 8000U;
static constexpr TickType_t kControllerScanRetryDelay = pdMS_TO_TICKS(1000);
static constexpr TickType_t kControllerCloseTimeout = pdMS_TO_TICKS(12000);
static constexpr uint32_t kControllerConnectTaskStackBytes = 6144U;
static constexpr UBaseType_t kControllerConnectTaskPriority = 2U;
static constexpr BaseType_t kControllerConnectTaskCore = 1;
static constexpr size_t kControllerNameBytes = 48U;

struct ControllerCandidate {
    uint8_t address[6] = {};
    uint8_t address_type = 0U;
    int8_t rssi = 0;
    uint16_t appearance = 0U;
    char name[kControllerNameBytes] = {};
};

static bool g_hidh_initialized = false;
static bool g_controller_scan_active = false;
static bool g_controller_stop_scan_requested = false;
static bool g_controller_candidate_ready = false;
static bool g_controller_connect_task_running = false;
static TickType_t g_controller_scan_due_tick = 0;
static ControllerCandidate g_controller_candidate = {};
static esp_hidh_dev_t *g_controller_dev = nullptr;
// 扫描诊断：每轮统计收到的广播条数，并只打印前若干条明细，避免刷屏。
static uint16_t g_controller_scan_report_count = 0;
static uint8_t g_controller_scan_log_budget = 0;

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
// The host only assembles bounded command bytes. The runtime pump owns replies.
static uint8_t g_ble_library_request[32] = {};
static uint8_t g_ble_library_received = 0;
static bool g_ble_library_request_ready = false;
static uint32_t g_ble_library_epoch = 0;
static bool g_ble_library_pumping = false;
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
        case 0x10U: return "循环模式";
        case 0x11U: return "音乐视图";
        case 0x12U: return "唤醒解锁";
        case 0x13U: return "锁屏";
        case 0x14U: return "AOD";
        case 0x15U: return "息屏";
        case 0x16U: return "磁带封面位置";
        case 0x17U: return "频谱样式";
        default: return "未知";
    }
}

static bool music_remote_ui_available()
{
    return app_manager_foreground() == AppId::Music &&
        !visual_music_app_remote_control_active() && !video_app_remote_control_active();
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
        case 0x10U: return value <= 3U && music_remote_ui_available() &&
            player_control_set_loop_mode(static_cast<PlayerLoopMode>(value));
        case 0x11U: return value <= 3U && music_remote_ui_available() && player_home_remote_request_view(value);
        case 0x12U: return music_remote_ui_available() && screen_lock_simple_remote_request(1U);
        case 0x13U: return music_remote_ui_available() && screen_lock_simple_remote_request(2U);
        case 0x14U: return music_remote_ui_available() && screen_lock_simple_remote_request(3U);
        case 0x15U: return music_remote_ui_available() && screen_lock_simple_remote_request(4U);
        case 0x16U: return value <= 1U && music_remote_ui_available() && player_home_remote_request_cover_shift(value);
        case 0x17U: return value <= 2U && music_remote_ui_available() && player_home_remote_request_spectrum_style(value);
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
    if (track_index != UINT32_MAX && player_state_is_ready() &&
        player_state_get_index() == track_index && player_state_get_track_view(&view)) {
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
        if (player_state_is_ready() && player_state_get_index() == track_index &&
            player_state_get_track_view(&view)) generation = view.generation;
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
    if (music_remote_ui_available()) flags |= 0x20U; // Music APP foreground
    if (player_state_get_source() == PlayerMediaSource::Nas ||
        (audio_valid && audio.source == AudioPlaybackSource::NasHttp)) flags |= 0x40U;
    if (!nsf_valid && !video_valid && music_remote_ui_available() && audio_valid &&
        player_control_local_seek_available(audio)) flags |= 0x80U;
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

// A 255-byte existing library frame is wrapped in <=20-byte BLE values.
// Keep the current fragment on notify backpressure; never advance on ENOMEM.
static void update_phone_library_notify()
{
    uint16_t conn;
    uint32_t epoch;
    portENTER_CRITICAL(&g_lock);
    const bool usable = g_connected && g_control_event_notify_enabled && !g_ble_library_pumping;
    conn = g_conn_handle; epoch = g_ble_library_epoch;
    if (usable) g_ble_library_pumping = true;
    portEXIT_CRITICAL(&g_lock);
    if (!usable) return;
    static uint32_t seen_epoch = UINT32_MAX;
    static uint8_t frame[255];
    static size_t frame_size = 0, offset = 0;
    if (seen_epoch != epoch) {
        seen_epoch = epoch; frame_size = offset = 0;
        library_view_remote_reset();
    }
    portENTER_CRITICAL(&g_lock);
    const bool request_waiting = g_ble_library_request_ready;
    portEXIT_CRITICAL(&g_lock);
    if (!request_waiting && frame_size == 0 && !library_view_remote_pending()) {
        portENTER_CRITICAL(&g_lock);g_ble_library_pumping = false;portEXIT_CRITICAL(&g_lock);
        return;
    }
    if (connection_is_encrypted(conn)) {
        uint8_t request[32]; bool queued;
        portENTER_CRITICAL(&g_lock);
        queued = g_ble_library_request_ready;
        if (queued) memcpy(request, g_ble_library_request, sizeof(request));
        portEXIT_CRITICAL(&g_lock);
        if (queued && frame_size == 0 && library_view_remote_submit(request, sizeof(request))) {
            portENTER_CRITICAL(&g_lock);
            if (epoch == g_ble_library_epoch) g_ble_library_request_ready = false;
            portEXIT_CRITICAL(&g_lock);
        }
        for (unsigned burst = 0; burst < 8; ++burst) {
            portENTER_CRITICAL(&g_lock);
            const bool current = epoch == g_ble_library_epoch && conn == g_conn_handle && g_control_event_notify_enabled;
            portEXIT_CRITICAL(&g_lock);
            if (!current) break;
            if (!frame_size) {
                if (!library_view_remote_take_frame(frame, &frame_size)) break;
                offset = 0;
            }
            size_t n = frame_size - offset;
            if (n > 17) n = 17;
            uint8_t packet[20] = {7, uint8_t(offset), uint8_t(frame_size)};
            memcpy(packet + 3, frame + offset, n);
            if (notify_phone_event(conn, packet, n + 3) != 0) break;
            offset += n;
            if (offset == frame_size) frame_size = offset = 0;
        }
    }
    portENTER_CRITICAL(&g_lock);g_ble_library_pumping = false;portEXIT_CRITICAL(&g_lock);
}

static int handle_library_fragment(const uint8_t *payload, size_t size)
{
    if (size != 18 || (payload[1] != 0 && payload[1] != 16)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    int result = 0;
    portENTER_CRITICAL(&g_lock);
    if (g_ble_library_request_ready) result = BLE_ATT_ERR_UNLIKELY;
    else {
        const uint8_t offset = payload[1];
        if (offset == 0) g_ble_library_received = 0;
        if (offset != g_ble_library_received) {g_ble_library_received = 0;result = BLE_ATT_ERR_UNLIKELY;}
        else {
            memcpy(g_ble_library_request + offset, payload + 2, 16);
            g_ble_library_received += 16;
            if (g_ble_library_received == 32) {
                // BLE may never export NAS data, even after a device-side source change.
                if (g_ble_library_request[0] != 0x30 || g_ble_library_request[1] != 1 ||
                    (g_ble_library_request[3] != 0 && g_ble_library_request[3] != 255)) result = BLE_ATT_ERR_UNLIKELY;
                else {g_ble_library_request[3] = 0;g_ble_library_request_ready = true;}
                g_ble_library_received = 0;
            }
        }
    }
    portEXIT_CRITICAL(&g_lock);
    return result;
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

static uint8_t g_last_remote_ui_status[6] = {0xFFU, 0, 0, 0, 0, 0};
static TickType_t g_remote_ui_resend_due = 0;
static uint8_t g_last_visual_status[4] = {0xFFU, 0, 0, 0};
static TickType_t g_visual_resend_due = 0;
static uint8_t g_last_seek_status[19] = {0xFFU};
static TickType_t g_seek_resend_due = 0;
static TickType_t g_aux_check_due = 0;

static void build_remote_ui_status(uint8_t packet[6])
{
    packet[0] = 0x03U; packet[1] = kPhoneEventProtocolVersion;
    packet[2] = static_cast<uint8_t>(player_control_get_loop_mode());
    packet[3] = player_home_remote_get_view();
    packet[4] = static_cast<uint8_t>(screen_lock_simple_get_power());
    packet[5] = static_cast<uint8_t>(screen_lock_simple_get_lock());
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
        g_control_event_val_handle == 0U) {
        return;
    }

    // Sample stable control snapshots at 100ms, but keep metadata fragmentation fast.
    if (!force_status && !force_metadata_refresh && g_aux_check_due != 0 && !tick_due(now, g_aux_check_due)) {
        if (g_metadata_chunk_index < g_metadata_chunk_count && connection_is_encrypted(conn_handle))
            (void)send_next_metadata_chunk(conn_handle);
        return;
    }
    if (!connection_is_encrypted(conn_handle)) return;
    g_aux_check_due = now + pdMS_TO_TICKS(100);
    uint8_t seek_status[19] = {};
    remote_music_seek_build_status(seek_status, music_remote_ui_available());
    if (memcmp(seek_status, g_last_seek_status, sizeof(seek_status)) != 0 ||
        g_seek_resend_due == 0 || tick_due(now, g_seek_resend_due)) {
        if (notify_phone_event(conn_handle, seek_status, sizeof(seek_status)) == 0) {
            memcpy(g_last_seek_status, seek_status, sizeof(seek_status));
            g_seek_resend_due = now + pdMS_TO_TICKS(15000);
        }
        return;
    }

    uint8_t ui_status[6] = {};
    build_remote_ui_status(ui_status);
    if (memcmp(ui_status, g_last_remote_ui_status, sizeof(ui_status)) != 0 ||
        g_remote_ui_resend_due == 0 || tick_due(now, g_remote_ui_resend_due)) {
        if (notify_phone_event(conn_handle, ui_status, sizeof(ui_status)) == 0) {
            memcpy(g_last_remote_ui_status, ui_status, sizeof(ui_status));
            g_remote_ui_resend_due = now + pdMS_TO_TICKS(15000);
        }
        return;
    }

    const uint8_t visual_status[4] = {
        0x04U, kPhoneEventProtocolVersion, 0x03U, player_home_remote_get_spectrum_style()};
    if (memcmp(visual_status, g_last_visual_status, sizeof(visual_status)) != 0 ||
        g_visual_resend_due == 0 || tick_due(now, g_visual_resend_due)) {
        if (notify_phone_event(conn_handle, visual_status, sizeof(visual_status)) == 0) {
            memcpy(g_last_visual_status, visual_status, sizeof(visual_status));
            g_visual_resend_due = now + pdMS_TO_TICKS(15000);
        }
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
    if (command == 0x31U) return handle_library_fragment(payload, payload_len);
    if (command == kWifiProvisionCommand) {
        return handle_wifi_provision_fragment(payload, payload_len);
    }

    if (command == 0x18U) {
        if (payload_len != 17U) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        const bool seek_ok = music_remote_ui_available() && remote_music_seek_execute(payload, payload_len);
        ESP_LOGI(TAG, "BLE本地音乐SEEK：result=%s", seek_ok ? "OK" : "REJECTED");
        return seek_ok ? 0 : BLE_ATT_ERR_UNLIKELY;
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
    } else if (command == 0x10U || command == 0x11U) {
        if (payload_len != 2U || payload[1] > 3U) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    } else if (command == 0x16U || command == 0x17U) {
        if (payload_len != 2U || payload[1] > (command == 0x16U ? 1U : 2U)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    } else if (payload_len != 1U ||
        !((command >= 0x01U && command <= 0x05U) ||
          (command >= 0x12U && command <= 0x15U))) {
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
                ++g_ble_library_epoch;g_ble_library_received = 0;g_ble_library_request_ready = false;
                g_control_event_notify_enabled = false;
                g_force_status_notify = false;
                g_force_metadata_refresh = false;
                g_status_notify_due_tick = 0;
                g_status_event_poll_due_tick = 0;
                g_last_seek_status[0] = 0xFFU;
                g_last_visual_status[0] = 0xFFU;
                g_last_remote_ui_status[0] = 0xFFU; // New BLE session must receive its own UI snapshot.
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
            ++g_ble_library_epoch;g_ble_library_received = 0;g_ble_library_request_ready = false;
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


static void host_reset_cb(int reason);
static void host_sync_cb();

static bool contains_ascii_case_insensitive(const char *text, const char *needle)
{
    if (text == nullptr || needle == nullptr || needle[0] == '\0') return false;
    const size_t needle_length = strlen(needle);
    for (const char *p = text; *p != '\0'; ++p) {
        size_t i = 0U;
        while (i < needle_length && p[i] != '\0' &&
               tolower(static_cast<unsigned char>(p[i])) ==
                   tolower(static_cast<unsigned char>(needle[i]))) {
            ++i;
        }
        if (i == needle_length) return true;
    }
    return false;
}

static bool parse_controller_candidate(
    const struct ble_gap_disc_desc *disc,
    ControllerCandidate *out_candidate)
{
    if (disc == nullptr || out_candidate == nullptr) return false;

    struct ble_hs_adv_fields fields = {};
    // 广播包末尾常有零长度填充（0x00），NimBLE 解析到填充会返回错误，
    // 但错误之前的字段已经解析完成并写入 fields，因此这里忽略返回值。
    (void)ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);

    bool has_hid_service = false;
    for (uint8_t i = 0U; i < fields.num_uuids16; ++i) {
        if (ble_uuid_u16(&fields.uuids16[i].u) == kHidServiceUuid16) {
            has_hid_service = true;
            break;
        }
    }

    ControllerCandidate candidate = {};
    memcpy(candidate.address, disc->addr.val, sizeof(candidate.address));
    candidate.address_type = disc->addr.type;
    candidate.rssi = disc->rssi;
    candidate.appearance = fields.appearance_is_present ? fields.appearance : 0U;

    if (fields.name != nullptr && fields.name_len > 0U) {
        size_t copy_length = fields.name_len;
        if (copy_length >= sizeof(candidate.name)) copy_length = sizeof(candidate.name) - 1U;
        memcpy(candidate.name, fields.name, copy_length);
        candidate.name[copy_length] = '\0';
    }

    // Prefer the standard Gamepad/Joystick appearance. Some controllers omit
    // appearance, so accept common controller names when HID service 0x1812 exists.
    // 很多手柄（Xbox / Switch Pro / 8BitDo）广播里根本不含 0x1812，只按 UUID 过滤会全部漏掉，
    // 因此 HID 服务、手柄外观、名称关键字三者满足其一即视为候选。
    bool gamepad_like = candidate.appearance == kGamepadAppearance ||
        candidate.appearance == kJoystickAppearance;
    if (!gamepad_like) {
        static const char *const kNameHints[] = {
            "gamepad", "game pad", "controller", "joystick", "wireless controller",
            "8bitdo", "gamesir", "ipega", "dualshock", "dualsense", "xbox", "pro controller"
        };
        for (const char *hint : kNameHints) {
            if (contains_ascii_case_insensitive(candidate.name, hint)) {
                gamepad_like = true;
                break;
            }
        }
    }
    if (!gamepad_like && !has_hid_service) return false;

    if (candidate.name[0] == '\0') {
        snprintf(candidate.name, sizeof(candidate.name), "%s", "BLE HID Gamepad");
    }
    *out_candidate = candidate;
    return true;
}

static void log_controller_adv_report(const struct ble_gap_disc_desc *disc)
{
    if (disc == nullptr) return;

    struct ble_hs_adv_fields fields = {};
    // 同上：零长度填充会让解析提前返回错误，但已解析字段仍然有效。
    (void)ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);

    char name[32] = {};
    uint16_t uuid16 = 0U;
    unsigned appearance = 0U;
    bool has_mfg = false;
    if (fields.name != nullptr && fields.name_len > 0U) {
        size_t copy_length = fields.name_len < sizeof(name) - 1U ? fields.name_len : sizeof(name) - 1U;
        memcpy(name, fields.name, copy_length);
        name[copy_length] = '\0';
    }
    if (fields.num_uuids16 > 0U) uuid16 = ble_uuid_u16(&fields.uuids16[0].u);
    if (fields.appearance_is_present) appearance = fields.appearance;
    has_mfg = fields.mfg_data != nullptr;

    const uint8_t *addr = disc->addr.val;
    char raw[3 * 31 + 1] = {};
    const uint8_t raw_length = disc->length_data < 31U ? disc->length_data : 31U;
    for (uint8_t i = 0U; i < raw_length; ++i) {
        snprintf(raw + i * 3U, 4U, "%02x ", disc->data[i]);
    }
    ESP_LOGI(TAG,
        "扫描广播：%02x:%02x:%02x:%02x:%02x:%02x type=%d rssi=%d name=\"%s\" app=0x%04x uuid16=0x%04x mfg=%s raw=%s",
        addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
        static_cast<int>(disc->event_type),
        static_cast<int>(disc->rssi),
        name,
        appearance,
        static_cast<unsigned>(uuid16),
        has_mfg ? "yes" : "no",
        raw);
}

static int controller_gap_event_cb(struct ble_gap_event *event, void *)
{
    if (event == nullptr) return 0;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            // 诊断：统计每轮收到的广播条数，并打印前若干条明细，
            // 用来区分“根本没收到广播”和“收到了但被过滤条件丢掉”。
            bool log_this_report = false;
            portENTER_CRITICAL(&g_lock);
            ++g_controller_scan_report_count;
            if (g_controller_scan_log_budget > 0U) {
                --g_controller_scan_log_budget;
                log_this_report = true;
            }
            portEXIT_CRITICAL(&g_lock);
            if (log_this_report) log_controller_adv_report(&event->disc);

            ControllerCandidate candidate = {};
            if (!parse_controller_candidate(&event->disc, &candidate)) return 0;

            bool accepted = false;
            portENTER_CRITICAL(&g_lock);
            if (g_desired_enabled &&
                g_mode == DeviceBluetoothMode::Controller &&
                g_active_mode == DeviceBluetoothMode::Controller &&
                !g_controller_candidate_ready &&
                !g_controller_connect_task_running &&
                g_state != BleRemoteState::Stopping) {
                g_controller_candidate = candidate;
                g_controller_candidate_ready = true;
                g_controller_scan_due_tick = 0;
                set_state_locked(BleRemoteState::ControllerConnecting);
                accepted = true;
            }
            portEXIT_CRITICAL(&g_lock);

            if (accepted) {
                ESP_LOGI(TAG,
                    "发现BLE手柄候选：name=%s appearance=0x%04x RSSI=%d addr_type=%u",
                    candidate.name,
                    static_cast<unsigned>(candidate.appearance),
                    static_cast<int>(candidate.rssi),
                    static_cast<unsigned>(candidate.address_type));
                // 不在 GAP 回调内直接取消扫描：改为置标志，由 update 在回调外调用
                // ble_gap_disc_cancel()，避免回调内重入导致 DISC_COMPLETE 丢失。
                portENTER_CRITICAL(&g_lock);
                g_controller_stop_scan_requested = true;
                portEXIT_CRITICAL(&g_lock);
            }
            return 0;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE: {
            portENTER_CRITICAL(&g_lock);
            g_controller_scan_active = false;
            const uint16_t report_count = g_controller_scan_report_count;
            if (g_desired_enabled &&
                g_mode == DeviceBluetoothMode::Controller &&
                g_state != BleRemoteState::Stopping &&
                !g_controller_connect_task_running) {
                // A matching candidate is consumed by the runtime pump after scan cancellation completes.
                set_state_locked(BleRemoteState::Scanning);
                g_controller_scan_due_tick = g_controller_candidate_ready
                    ? 0 : xTaskGetTickCount() + kControllerScanRetryDelay;
            }
            portEXIT_CRITICAL(&g_lock);
            ESP_LOGI(TAG, "BLE手柄扫描轮次结束：reason=%d 收到广播=%u",
                event->disc_complete.reason, static_cast<unsigned>(report_count));
            return 0;
        }

        default:
            return 0;
    }
}

static int start_controller_scan()
{
    if (!desired_enabled() || g_active_mode != DeviceBluetoothMode::Controller) {
        return 0;
    }

    struct ble_gap_disc_params params = {};
    params.filter_duplicates = 1;
    params.passive = 0;
    params.itvl = 0x50;
    params.window = 0x30;
    params.filter_policy = 0;
    params.limited = 0;

    portENTER_CRITICAL(&g_lock);
    if (g_controller_scan_active || g_controller_connect_task_running ||
        g_controller_candidate_ready || g_state == BleRemoteState::Stopping) {
        portEXIT_CRITICAL(&g_lock);
        return 0;
    }
    g_controller_scan_active = true;
    g_controller_stop_scan_requested = false;
    g_controller_scan_due_tick = 0;
    g_controller_scan_report_count = 0;
    g_controller_scan_log_budget = 10;
    set_state_locked(BleRemoteState::Scanning);
    portEXIT_CRITICAL(&g_lock);

    const int rc = ble_gap_disc(
        g_own_addr_type,
        kControllerScanDurationMs,
        &params,
        controller_gap_event_cb,
        nullptr);
    if (rc != 0) {
        portENTER_CRITICAL(&g_lock);
        g_controller_scan_active = false;
        if (g_desired_enabled && g_mode == DeviceBluetoothMode::Controller) {
            set_state_locked(BleRemoteState::RetryWait, ESP_FAIL);
            g_retry_due_tick = xTaskGetTickCount() + kRetryDelay;
        }
        portEXIT_CRITICAL(&g_lock);
        ESP_LOGW(TAG, "BLE手柄扫描启动失败：rc=%d", rc);
        return rc;
    }

    ESP_LOGI(TAG, "BLE手柄扫描已启动：窗口=%ums，过滤HID Gamepad/Joystick",
        static_cast<unsigned>(kControllerScanDurationMs));
    return 0;
}

static void controller_connect_task(void *)
{
    ControllerCandidate candidate = {};
    portENTER_CRITICAL(&g_lock);
    candidate = g_controller_candidate;
    portEXIT_CRITICAL(&g_lock);

    ESP_LOGI(TAG, "连接BLE手柄：name=%s RSSI=%d",
        candidate.name, static_cast<int>(candidate.rssi));

    esp_hidh_dev_t *dev = esp_hidh_dev_open(
        candidate.address,
        ESP_HID_TRANSPORT_BLE,
        candidate.address_type);

    const bool dev_exists = dev != nullptr && esp_hidh_dev_exists(dev);
    portENTER_CRITICAL(&g_lock);
    g_controller_connect_task_running = false;
    if (dev_exists && g_controller_dev == nullptr) {
        // OPEN_EVENT normally stores this first; this is a race-safe fallback.
        g_controller_dev = dev;
    }
    if (!dev_exists &&
        g_desired_enabled &&
        g_mode == DeviceBluetoothMode::Controller &&
        g_state != BleRemoteState::Stopping) {
        g_connected = false;
        set_state_locked(BleRemoteState::Scanning);
        g_controller_scan_due_tick = xTaskGetTickCount() + kControllerScanRetryDelay;
    }
    portEXIT_CRITICAL(&g_lock);

    if (!dev_exists) {
        ESP_LOGW(TAG, "BLE手柄连接失败：稍后重新扫描");
    }
    vTaskDelete(nullptr);
}

static bool start_controller_connect_task()
{
    portENTER_CRITICAL(&g_lock);
    if (!g_controller_candidate_ready ||
        g_controller_scan_active ||
        g_controller_connect_task_running ||
        !g_desired_enabled ||
        g_mode != DeviceBluetoothMode::Controller ||
        g_state == BleRemoteState::Stopping) {
        portEXIT_CRITICAL(&g_lock);
        return false;
    }
    g_controller_candidate_ready = false;
    g_controller_stop_scan_requested = false;
    g_controller_connect_task_running = true;
    set_state_locked(BleRemoteState::ControllerConnecting);
    portEXIT_CRITICAL(&g_lock);

    const BaseType_t created = xTaskCreatePinnedToCore(
        controller_connect_task,
        "ble_hid_open",
        kControllerConnectTaskStackBytes,
        nullptr,
        kControllerConnectTaskPriority,
        nullptr,
        kControllerConnectTaskCore);
    if (created == pdPASS) return true;

    portENTER_CRITICAL(&g_lock);
    g_controller_connect_task_running = false;
    if (g_desired_enabled && g_mode == DeviceBluetoothMode::Controller) {
        set_state_locked(BleRemoteState::Scanning, ESP_ERR_NO_MEM);
        g_controller_scan_due_tick = xTaskGetTickCount() + kControllerScanRetryDelay;
    }
    portEXIT_CRITICAL(&g_lock);
    ESP_LOGW(TAG, "BLE手柄连接任务创建失败：NO_MEM");
    return false;
}

static void gamepad_hidh_event_cb(
    void *,
    esp_event_base_t,
    int32_t event_id,
    void *event_data)
{
    if (event_data == nullptr) return;
    const esp_hidh_event_t event = static_cast<esp_hidh_event_t>(event_id);
    esp_hidh_event_data_t *param = static_cast<esp_hidh_event_data_t *>(event_data);

    switch (event) {
        case ESP_HIDH_OPEN_EVENT: {
            esp_hidh_dev_t *dev = param->open.dev;
            if (param->open.status != ESP_OK || dev == nullptr) {
                portENTER_CRITICAL(&g_lock);
                g_controller_connect_task_running = false;
                if (g_desired_enabled &&
                    g_mode == DeviceBluetoothMode::Controller &&
                    g_state != BleRemoteState::Stopping) {
                    set_state_locked(BleRemoteState::Scanning);
                    g_controller_scan_due_tick = xTaskGetTickCount() + kControllerScanRetryDelay;
                }
                portEXIT_CRITICAL(&g_lock);
                ESP_LOGW(TAG, "BLE HID手柄打开失败：%s", esp_err_to_name(param->open.status));
                break;
            }

            const char *name = esp_hidh_dev_name_get(dev);
            const uint16_t vendor_id = esp_hidh_dev_vendor_id_get(dev);
            const uint16_t product_id = esp_hidh_dev_product_id_get(dev);
            const uint8_t usage = static_cast<uint8_t>(esp_hidh_dev_usage_get(dev));
            size_t map_count = 0U;
            esp_hid_raw_report_map_t *maps = nullptr;
            (void)esp_hidh_dev_report_maps_get(dev, &map_count, &maps);
            const uint8_t *map_data = nullptr;
            size_t map_length = 0U;
            if (maps != nullptr && map_count > 0U) {
                map_data = maps[0].data;
                map_length = maps[0].len;
            }

            portENTER_CRITICAL(&g_lock);
            g_controller_dev = dev;
            g_controller_connect_task_running = false;
            g_connected = true;
            set_state_locked(BleRemoteState::ControllerConnected);
            portEXIT_CRITICAL(&g_lock);

            gamepad_input_service_set_connected(
                name, vendor_id, product_id, usage, map_data, map_length);
            ESP_LOGI(TAG,
                "BLE手柄已连接：name=%s VID=0x%04x PID=0x%04x usage=%u maps=%u map0=%uB",
                name != nullptr ? name : "Unknown",
                static_cast<unsigned>(vendor_id),
                static_cast<unsigned>(product_id),
                static_cast<unsigned>(usage),
                static_cast<unsigned>(map_count),
                static_cast<unsigned>(map_length));
            break;
        }

        case ESP_HIDH_BATTERY_EVENT:
            gamepad_input_service_set_battery(param->battery.level);
            ESP_LOGI(TAG, "BLE手柄电量：%u%%",
                static_cast<unsigned>(param->battery.level));
            break;

        case ESP_HIDH_INPUT_EVENT:
            gamepad_input_service_submit_report(
                static_cast<uint8_t>(param->input.usage),
                param->input.report_id,
                param->input.map_index,
                param->input.data,
                param->input.length);
            break;

        case ESP_HIDH_CLOSE_EVENT: {
            portENTER_CRITICAL(&g_lock);
            if (g_controller_dev == param->close.dev) {
                g_controller_dev = nullptr;
            }
            g_controller_connect_task_running = false;
            g_connected = false;
            if (g_desired_enabled &&
                g_mode == DeviceBluetoothMode::Controller &&
                g_state != BleRemoteState::Stopping) {
                set_state_locked(BleRemoteState::Scanning);
                g_controller_scan_due_tick = xTaskGetTickCount() + kControllerScanRetryDelay;
            }
            portEXIT_CRITICAL(&g_lock);
            gamepad_input_service_set_disconnected();
            ESP_LOGI(TAG, "BLE手柄已断开：reason=%d", param->close.reason);
            break;
        }

        default:
            break;
    }
}

static esp_err_t init_controller_hid_host()
{
    esp_hidh_config_t config = {};
    config.callback = gamepad_hidh_event_cb;
    config.event_stack_size = 4096U;
    config.callback_arg = nullptr;

    const esp_err_t ret = esp_hidh_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-IDF HID Host初始化失败：%s", esp_err_to_name(ret));
        return ret;
    }
    g_hidh_initialized = true;

    // esp_hidh_init() installs its own no-op NimBLE sync/reset callbacks.
    // Restore FakePod's callbacks afterwards so phone advertising or controller
    // scanning is started by the same host lifecycle callback.
    ble_hs_cfg.reset_cb = host_reset_cb;
    ble_hs_cfg.sync_cb = host_sync_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ESP_LOGI(TAG, "ESP-IDF NimBLE HID Host已初始化");
    return ESP_OK;
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

    if (rc == 0) {
        if (g_active_mode == DeviceBluetoothMode::Controller) {
            rc = start_controller_scan();
        } else {
            rc = start_advertising();
        }
    }

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

    if (g_active_mode == DeviceBluetoothMode::Controller) {
        ESP_LOGI(TAG, "BLE Central已就绪：开始扫描Generic HID手柄");
    } else {
        ESP_LOGI(TAG, "BLE广播已启动：name=%s interval≈500ms", kDeviceName);
    }
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

    if (g_active_mode == DeviceBluetoothMode::Controller) {
        if (g_controller_scan_active) {
            const int cancel_rc = ble_gap_disc_cancel();
            if (cancel_rc != 0 && cancel_rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "停止手柄扫描失败：rc=%d", cancel_rc);
            }
        }

        const TickType_t close_started = xTaskGetTickCount();
        while (g_controller_connect_task_running &&
               xTaskGetTickCount() - close_started < pdMS_TO_TICKS(32000)) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (g_controller_connect_task_running) {
            ESP_LOGE(TAG, "HID连接任务未退出，拒绝停止NimBLE以避免释放竞态");
            return ESP_ERR_TIMEOUT;
        }

        esp_hidh_dev_t *dev = nullptr;
        portENTER_CRITICAL(&g_lock);
        dev = g_controller_dev;
        portEXIT_CRITICAL(&g_lock);
        if (dev != nullptr && esp_hidh_dev_exists(dev)) {
            const esp_err_t close_ret = esp_hidh_dev_close(dev);
            if (close_ret != ESP_OK && close_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "请求关闭HID手柄失败：%s", esp_err_to_name(close_ret));
            }
            const TickType_t started = xTaskGetTickCount();
            while (g_controller_dev != nullptr &&
                   xTaskGetTickCount() - started < kControllerCloseTimeout) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (g_controller_dev != nullptr) {
                ESP_LOGE(TAG, "HID手柄未在超时内完成释放，拒绝停止NimBLE");
                return ESP_ERR_TIMEOUT;
            }
            // CLOSE callback clears the pointer before esp_hidh's postprocessor frees it.
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (g_hidh_initialized) {
            const esp_err_t hidh_ret = esp_hidh_deinit();
            if (hidh_ret != ESP_OK) {
                ESP_LOGE(TAG, "ESP-IDF HID Host反初始化失败：%s", esp_err_to_name(hidh_ret));
                return hidh_ret;
            }
            g_hidh_initialized = false;
        }
        gamepad_input_service_set_disconnected();
    }

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
    g_active_mode = DeviceBluetoothMode::Off;
    g_controller_scan_active = false;
    g_controller_candidate_ready = false;
    g_controller_connect_task_running = false;
    g_controller_scan_due_tick = 0;
    g_controller_dev = nullptr;
    portEXIT_CRITICAL(&g_lock);
    gamepad_input_service_set_disconnected();
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
    g_active_mode = g_mode;
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

    if (g_active_mode == DeviceBluetoothMode::PhoneRemote) {
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
    } else if (g_active_mode == DeviceBluetoothMode::Controller) {
        const int appearance_rc = ble_svc_gap_device_appearance_set(0U);
        if (appearance_rc != 0) {
            ESP_LOGW(TAG, "BLE Central外观配置失败：rc=%d", appearance_rc);
            (void)stop_stack();
            return ESP_FAIL;
        }
    } else {
        (void)stop_stack();
        return ESP_ERR_INVALID_STATE;
    }

    const int name_rc = ble_svc_gap_device_name_set(kDeviceName);
    if (name_rc != 0) {
        ESP_LOGW(TAG, "BLE设备名设置失败：rc=%d", name_rc);
        (void)stop_stack();
        return ESP_FAIL;
    }

    // NimBLE 安全存储接入默认 NVS，使 Bond/LTK 在设备重启后仍可恢复。
    ble_store_config_init();

    if (g_active_mode == DeviceBluetoothMode::Controller) {
        const esp_err_t hidh_ret = init_controller_hid_host();
        if (hidh_ret != ESP_OK) {
            (void)stop_stack();
            return hidh_ret;
        }
        gamepad_input_service_reset();
        ESP_LOGI(TAG, "BLE手柄模式：NimBLE Central + ESP-IDF HID Host；不注册手机GATT服务");
    } else {
        ESP_LOGI(TAG, "BLE手机控制服务已注册：6媒体命令 + Wi-Fi配网分片 + 状态Notify V1");
        ESP_LOGI(TAG, "BLE安全：Bond=ON SC=ON MITM=OFF KeyDist=ENC|ID");
    }
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
    ESP_LOGI(TAG, "BLE Foundation就绪：NimBLE Peripheral/GATT Server + Central/Observer，默认关闭");
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
    if (enabled && g_mode == DeviceBluetoothMode::Off) {
        g_mode = DeviceBluetoothMode::PhoneRemote;
    }
    g_desired_enabled = enabled;
    g_retry_due_tick = 0;
    portEXIT_CRITICAL(&g_lock);
    ESP_LOGI(TAG, "BLE用户意图：%s", enabled ? "开启" : "关闭");
}

void ble_remote_service_set_mode(DeviceBluetoothMode mode)
{
    if (mode != DeviceBluetoothMode::Off &&
        mode != DeviceBluetoothMode::PhoneRemote &&
        mode != DeviceBluetoothMode::Controller) return;
    portENTER_CRITICAL(&g_lock);
    if (!g_ready) {
        portEXIT_CRITICAL(&g_lock);
        return;
    }
    g_mode = mode;
    g_desired_enabled = mode != DeviceBluetoothMode::Off;
    g_retry_due_tick = 0;
    portEXIT_CRITICAL(&g_lock);
    if (mode == DeviceBluetoothMode::Off) gamepad_input_service_set_disconnected();
    ESP_LOGI(TAG, "BLE工作模式：%s", device_settings_bluetooth_mode_name(mode));
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
        DeviceBluetoothMode mode = DeviceBluetoothMode::PhoneRemote;
        portENTER_CRITICAL(&g_lock);
        mode = g_mode;
        portEXIT_CRITICAL(&g_lock);
        if (mode == DeviceBluetoothMode::Controller) {
            if (state == BleRemoteState::ControllerConnected) return;
            if (state == BleRemoteState::ControllerConnecting) {
                // 兜底：若 DISC_COMPLETE 丢失或连接任务未起来，避免永久卡在“连接手柄”。
                bool connect_running = false;
                bool candidate_ready = false;
                bool scan_active = false;
                bool stop_requested = false;
                portENTER_CRITICAL(&g_lock);
                connect_running = g_controller_connect_task_running;
                candidate_ready = g_controller_candidate_ready;
                scan_active = g_controller_scan_active;
                stop_requested = g_controller_stop_scan_requested;
                portEXIT_CRITICAL(&g_lock);
                if (connect_running) return;
                if (!candidate_ready) {
                    bool fell_back = false;
                    portENTER_CRITICAL(&g_lock);
                    if (g_state == BleRemoteState::ControllerConnecting) {
                        set_state_locked(BleRemoteState::Scanning);
                        g_controller_scan_due_tick = now + kControllerScanRetryDelay;
                        fell_back = true;
                    }
                    portEXIT_CRITICAL(&g_lock);
                    if (fell_back) ESP_LOGW(TAG, "BLE手柄连接任务未启动：回退到扫描");
                    return;
                }
                if (scan_active) {
                    if (stop_requested) {
                        // 在 GAP 回调之外取消扫描，避免回调内重入丢失 DISC_COMPLETE。
                        portENTER_CRITICAL(&g_lock);
                        g_controller_stop_scan_requested = false;
                        portEXIT_CRITICAL(&g_lock);
                        const int cancel_rc = ble_gap_disc_cancel();
                        if (cancel_rc != 0 && cancel_rc != BLE_HS_EALREADY) {
                            ESP_LOGW(TAG, "停止手柄扫描失败：rc=%d", cancel_rc);
                        }
                        return;
                    }
                    if ((now - state_since) < pdMS_TO_TICKS(1000)) return;
                    portENTER_CRITICAL(&g_lock);
                    g_controller_scan_active = false;
                    portEXIT_CRITICAL(&g_lock);
                    ESP_LOGW(TAG, "手柄扫描停止确认超时：强制进入连接");
                }
                (void)start_controller_connect_task();
                return;
            }
            if (state == BleRemoteState::Scanning) {
                bool scan_active = false;
                bool candidate_ready = false;
                bool connect_running = false;
                TickType_t scan_due = 0;
                portENTER_CRITICAL(&g_lock);
                scan_active = g_controller_scan_active;
                candidate_ready = g_controller_candidate_ready;
                connect_running = g_controller_connect_task_running;
                scan_due = g_controller_scan_due_tick;
                portEXIT_CRITICAL(&g_lock);
                if (candidate_ready && !scan_active && !connect_running) {
                    (void)start_controller_connect_task();
                } else if (!scan_active && !connect_running &&
                           (scan_due == 0 || tick_due(now, scan_due))) {
                    (void)start_controller_scan();
                }
                return;
            }
        } else if (state == BleRemoteState::Connected) {
#if FAKEPOD_BLE_FOUNDATION_ENABLED
            update_phone_state_notify(now);
            update_phone_library_notify();
#endif
            return;
        }
        if (mode == DeviceBluetoothMode::PhoneRemote && state == BleRemoteState::Advertising) return;
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
        case BleRemoteState::Scanning: return "扫描手柄";
        case BleRemoteState::ControllerConnecting: return "连接手柄";
        case BleRemoteState::ControllerConnected: return "手柄已连接";
        case BleRemoteState::Connected: return "手机已连接";
        case BleRemoteState::Stopping: return "关闭中";
        case BleRemoteState::RetryWait: return "故障重试";
        case BleRemoteState::Unsupported: return "未启用NimBLE";
        default: return "未知";
    }
}
