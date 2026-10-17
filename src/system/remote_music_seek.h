#pragma once
#include <stddef.h>
#include <stdint.h>
#include "audio_service.h"
#include "player_control.h"

// Optional V1 extension; existing status packets are unchanged in size.
static inline uint32_t remote_seek_read_u32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
        (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
}
static inline void remote_seek_write_u32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4U; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8U));
}
static inline bool remote_music_seek_execute(const uint8_t *payload, size_t size)
{
    return payload != nullptr && size == 17U && payload[0] == 0x18U &&
        player_control_remote_seek_ms(remote_seek_read_u32(payload + 1),
            remote_seek_read_u32(payload + 5), remote_seek_read_u32(payload + 9),
            remote_seek_read_u32(payload + 13));
}
static inline void remote_music_seek_build_status(uint8_t packet[19], bool music_available)
{
    AudioStateSnapshot audio = {};
    const bool valid = audio_service_get_snapshot(&audio) && audio.ready;
    packet[0] = 0x05U; packet[1] = 1U;
    packet[2] = 0x01U | ((valid && music_available && player_control_local_seek_available(audio)) ? 0x02U : 0U);
    remote_seek_write_u32(packet + 3, valid ? audio.track_index : UINT32_MAX);
    remote_seek_write_u32(packet + 7, valid ? audio.playback_revision : 0U);
    remote_seek_write_u32(packet + 11, valid ? audio.seek_revision : 0U);
    remote_seek_write_u32(packet + 15, static_cast<uint32_t>(audio.last_seek_target_ms));
}
