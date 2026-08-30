#include "visual_music_midi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/storage/storage_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "电子音流MIDI";

namespace VisualMusicMidi
{
namespace
{

static constexpr size_t kReadChunkBytes = 4096U;
static constexpr TickType_t kStorageLockTimeout = pdMS_TO_TICKS(30);
static constexpr uint32_t kTaskStack = 7168U;
static constexpr UBaseType_t kTaskPriority = 1U;
static constexpr BaseType_t kTaskCore = 0;

struct TaskArgs
{
    char *path = nullptr; // PSRAM
    uint32_t generation = 0U;
};

enum class RawEventType : uint8_t
{
    NoteOff = 0U,
    NoteOn,
    ProgramChange,
};

struct RawEvent
{
    uint64_t tick = 0U;
    uint32_t sequence = 0U;
    uint16_t track = 0U;
    uint8_t note = 0U;
    uint8_t velocity = 0U;
    uint8_t channel = 0U;
    uint8_t program = 0U;
    RawEventType type = RawEventType::NoteOff;
    uint8_t reserved = 0U;
};

struct TempoEvent
{
    uint64_t tick = 0U;
    uint32_t usec_per_quarter = 500000U;
    uint32_t sequence = 0U;
};
static_assert(sizeof(TempoEvent) == 16U, "TempoEvent must remain compact");

struct ParseScratch
{
    RawEvent *events = nullptr;
    size_t event_count = 0U;
    size_t event_capacity = 0U;
    TempoEvent *tempos = nullptr;
    size_t tempo_count = 0U;
    size_t tempo_capacity = 0U;
    uint32_t sequence = 0U;
    uint64_t max_tick = 0U;
    size_t note_on_count = 0U;
};

struct ActiveNote
{
    uint32_t start_ms = 0U;
    uint16_t track = 0U;
    uint8_t note = 0U;
    uint8_t velocity = 0U;
    uint8_t channel = 0U;
    uint8_t program = 0U;
    uint8_t reserved[2] = {};
};
static_assert(sizeof(ActiveNote) == 12U, "ActiveNote must remain compact");

static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_generation = 1U;
static bool g_initialized = false;
static bool g_result_pending = false;
static LoadResult g_pending_result = {};

static bool generation_current(uint32_t generation)
{
    bool current = false;
    portENTER_CRITICAL(&g_mux);
    current = generation == g_generation;
    portEXIT_CRITICAL(&g_mux);
    return current;
}

static uint16_t read_be16(const uint8_t *p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8U) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24U) |
        (static_cast<uint32_t>(p[1]) << 16U) |
        (static_cast<uint32_t>(p[2]) << 8U) |
        static_cast<uint32_t>(p[3]);
}

static bool read_vlq(const uint8_t *data, size_t size, size_t *io_pos, uint32_t *out_value)
{
    if (data == nullptr || io_pos == nullptr || out_value == nullptr) return false;
    uint32_t value = 0U;
    for (uint8_t i = 0U; i < 4U; ++i) {
        if (*io_pos >= size) return false;
        const uint8_t byte = data[(*io_pos)++];
        value = (value << 7U) | static_cast<uint32_t>(byte & 0x7FU);
        if ((byte & 0x80U) == 0U) {
            *out_value = value;
            return true;
        }
    }
    return false;
}

static bool grow_psram(void **block, size_t old_bytes, size_t new_bytes)
{
    if (block == nullptr || new_bytes <= old_bytes) return false;
    void *next = heap_caps_malloc(new_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (next == nullptr) return false;
    if (*block != nullptr && old_bytes > 0U) memcpy(next, *block, old_bytes);
    if (*block != nullptr) heap_caps_free(*block);
    *block = next;
    return true;
}

static esp_err_t reserve_events(ParseScratch *scratch, size_t needed)
{
    if (scratch == nullptr) return ESP_ERR_INVALID_ARG;
    if (needed <= scratch->event_capacity) return ESP_OK;
    size_t capacity = scratch->event_capacity > 0U ? scratch->event_capacity : 512U;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) return ESP_ERR_INVALID_SIZE;
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(RawEvent)) return ESP_ERR_INVALID_SIZE;
    void *block = scratch->events;
    if (!grow_psram(
            &block,
            scratch->event_capacity * sizeof(RawEvent),
            capacity * sizeof(RawEvent))) {
        return ESP_ERR_NO_MEM;
    }
    scratch->events = static_cast<RawEvent *>(block);
    scratch->event_capacity = capacity;
    return ESP_OK;
}

static esp_err_t reserve_tempos(ParseScratch *scratch, size_t needed)
{
    if (scratch == nullptr) return ESP_ERR_INVALID_ARG;
    if (needed <= scratch->tempo_capacity) return ESP_OK;
    size_t capacity = scratch->tempo_capacity > 0U ? scratch->tempo_capacity : 16U;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) return ESP_ERR_INVALID_SIZE;
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(TempoEvent)) return ESP_ERR_INVALID_SIZE;
    void *block = scratch->tempos;
    if (!grow_psram(
            &block,
            scratch->tempo_capacity * sizeof(TempoEvent),
            capacity * sizeof(TempoEvent))) {
        return ESP_ERR_NO_MEM;
    }
    scratch->tempos = static_cast<TempoEvent *>(block);
    scratch->tempo_capacity = capacity;
    return ESP_OK;
}

static esp_err_t append_note_edge(
    ParseScratch *scratch,
    uint64_t tick,
    uint16_t track,
    uint8_t channel,
    uint8_t note,
    uint8_t velocity,
    bool is_on)
{
    const esp_err_t reserve = reserve_events(scratch, scratch->event_count + 1U);
    if (reserve != ESP_OK) return reserve;
    RawEvent &event = scratch->events[scratch->event_count++];
    event.tick = tick;
    event.sequence = scratch->sequence++;
    event.track = track;
    event.note = note;
    event.velocity = velocity;
    event.channel = channel;
    event.type = is_on ? RawEventType::NoteOn : RawEventType::NoteOff;
    if (is_on) ++scratch->note_on_count;
    if (tick > scratch->max_tick) scratch->max_tick = tick;
    return ESP_OK;
}

static esp_err_t append_program_change(
    ParseScratch *scratch,
    uint64_t tick,
    uint16_t track,
    uint8_t channel,
    uint8_t program)
{
    const esp_err_t reserve = reserve_events(scratch, scratch->event_count + 1U);
    if (reserve != ESP_OK) return reserve;
    RawEvent &event = scratch->events[scratch->event_count++];
    event.tick = tick;
    event.sequence = scratch->sequence++;
    event.track = track;
    event.channel = channel;
    event.program = program;
    event.type = RawEventType::ProgramChange;
    if (tick > scratch->max_tick) scratch->max_tick = tick;
    return ESP_OK;
}

static esp_err_t append_tempo(
    ParseScratch *scratch,
    uint64_t tick,
    uint32_t usec_per_quarter)
{
    if (usec_per_quarter == 0U) return ESP_ERR_INVALID_RESPONSE;
    const esp_err_t reserve = reserve_tempos(scratch, scratch->tempo_count + 1U);
    if (reserve != ESP_OK) return reserve;
    TempoEvent &event = scratch->tempos[scratch->tempo_count++];
    event.tick = tick;
    event.usec_per_quarter = usec_per_quarter;
    event.sequence = scratch->sequence++;
    if (tick > scratch->max_tick) scratch->max_tick = tick;
    return ESP_OK;
}

static esp_err_t parse_track(
    const uint8_t *data,
    size_t size,
    uint16_t track_index,
    uint32_t generation,
    ParseScratch *scratch)
{
    if (data == nullptr || scratch == nullptr) return ESP_ERR_INVALID_ARG;
    size_t pos = 0U;
    uint64_t tick = 0U;
    uint8_t running_status = 0U;
    uint32_t parsed_events = 0U;

    while (pos < size) {
        if ((parsed_events++ & 0xFFU) == 0U && !generation_current(generation)) {
            return ESP_ERR_INVALID_STATE;
        }
        uint32_t delta = 0U;
        if (!read_vlq(data, size, &pos, &delta)) return ESP_ERR_INVALID_RESPONSE;
        if (UINT64_MAX - tick < delta) return ESP_ERR_INVALID_SIZE;
        tick += delta;
        if (tick > scratch->max_tick) scratch->max_tick = tick;
        if (pos >= size) return ESP_ERR_INVALID_RESPONSE;

        uint8_t status = data[pos++];
        bool data_byte_consumed = false;
        uint8_t first_data = 0U;
        if (status < 0x80U) {
            if (running_status < 0x80U || running_status >= 0xF0U) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            first_data = status;
            status = running_status;
            data_byte_consumed = true;
        } else if (status < 0xF0U) {
            running_status = status;
        }

        if (status == 0xFFU) {
            if (pos >= size) return ESP_ERR_INVALID_RESPONSE;
            const uint8_t meta_type = data[pos++];
            uint32_t length = 0U;
            if (!read_vlq(data, size, &pos, &length) || length > size - pos) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (meta_type == 0x51U && length == 3U) {
                const uint32_t tempo =
                    (static_cast<uint32_t>(data[pos]) << 16U) |
                    (static_cast<uint32_t>(data[pos + 1U]) << 8U) |
                    static_cast<uint32_t>(data[pos + 2U]);
                const esp_err_t ret = append_tempo(scratch, tick, tempo);
                if (ret != ESP_OK) return ret;
            }
            const bool end_track = meta_type == 0x2FU;
            pos += length;
            running_status = 0U;
            if (end_track) break;
            continue;
        }

        if (status == 0xF0U || status == 0xF7U) {
            uint32_t length = 0U;
            if (!read_vlq(data, size, &pos, &length) || length > size - pos) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            pos += length;
            running_status = 0U;
            continue;
        }

        if (status >= 0xF0U) return ESP_ERR_NOT_SUPPORTED;

        const uint8_t kind = status & 0xF0U;
        const uint8_t channel = status & 0x0FU;
        const uint8_t data_count = (kind == 0xC0U || kind == 0xD0U) ? 1U : 2U;
        uint8_t d1 = 0U;
        uint8_t d2 = 0U;
        if (data_byte_consumed) {
            d1 = first_data;
        } else {
            if (pos >= size) return ESP_ERR_INVALID_RESPONSE;
            d1 = data[pos++];
        }
        if (data_count == 2U) {
            if (pos >= size) return ESP_ERR_INVALID_RESPONSE;
            d2 = data[pos++];
        }
        if ((d1 & 0x80U) != 0U || (data_count == 2U && (d2 & 0x80U) != 0U)) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (kind == 0x80U) {
            const esp_err_t ret = append_note_edge(scratch, tick, track_index, channel, d1, d2, false);
            if (ret != ESP_OK) return ret;
        } else if (kind == 0x90U) {
            const bool on = d2 != 0U;
            const esp_err_t ret = append_note_edge(scratch, tick, track_index, channel, d1, d2, on);
            if (ret != ESP_OK) return ret;
        } else if (kind == 0xC0U) {
            const esp_err_t ret = append_program_change(scratch, tick, track_index, channel, d1);
            if (ret != ESP_OK) return ret;
        }
    }

    return ESP_OK;
}

static int raw_event_compare(const void *lhs_ptr, const void *rhs_ptr)
{
    const RawEvent &lhs = *static_cast<const RawEvent *>(lhs_ptr);
    const RawEvent &rhs = *static_cast<const RawEvent *>(rhs_ptr);
    if (lhs.tick != rhs.tick) return lhs.tick < rhs.tick ? -1 : 1;
    if (lhs.sequence != rhs.sequence) return lhs.sequence < rhs.sequence ? -1 : 1;
    return 0;
}

static int tempo_compare(const void *lhs_ptr, const void *rhs_ptr)
{
    const TempoEvent &lhs = *static_cast<const TempoEvent *>(lhs_ptr);
    const TempoEvent &rhs = *static_cast<const TempoEvent *>(rhs_ptr);
    if (lhs.tick != rhs.tick) return lhs.tick < rhs.tick ? -1 : 1;
    if (lhs.sequence != rhs.sequence) return lhs.sequence < rhs.sequence ? -1 : 1;
    return 0;
}

static int note_compare(const void *lhs_ptr, const void *rhs_ptr)
{
    const NoteEvent &lhs = *static_cast<const NoteEvent *>(lhs_ptr);
    const NoteEvent &rhs = *static_cast<const NoteEvent *>(rhs_ptr);
    if (lhs.start_ms != rhs.start_ms) return lhs.start_ms < rhs.start_ms ? -1 : 1;
    if (lhs.note != rhs.note) return lhs.note < rhs.note ? -1 : 1;
    if (lhs.channel != rhs.channel) return lhs.channel < rhs.channel ? -1 : 1;
    return 0;
}

static uint64_t ticks_to_usec(uint64_t ticks, uint32_t tempo, uint16_t division)
{
    if (division == 0U || tempo == 0U) return 0U;
    if (ticks > UINT64_MAX / static_cast<uint64_t>(tempo)) return UINT64_MAX;
    return (ticks * static_cast<uint64_t>(tempo)) / static_cast<uint64_t>(division);
}

static uint32_t clamp_ms(uint64_t usec)
{
    const uint64_t ms = usec / 1000ULL;
    return ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ms);
}

static esp_err_t build_timeline(
    ParseScratch *scratch,
    uint16_t division,
    uint16_t track_count,
    uint8_t format,
    uint32_t generation,
    Timeline *out)
{
    if (scratch == nullptr || out == nullptr || division == 0U) return ESP_ERR_INVALID_ARG;
    if (scratch->event_count == 0U || scratch->note_on_count == 0U) return ESP_ERR_NOT_FOUND;

    qsort(scratch->events, scratch->event_count, sizeof(RawEvent), raw_event_compare);
    if (scratch->tempo_count > 1U) {
        qsort(scratch->tempos, scratch->tempo_count, sizeof(TempoEvent), tempo_compare);
    }

    NoteEvent *notes = static_cast<NoteEvent *>(heap_caps_calloc(
        scratch->note_on_count,
        sizeof(NoteEvent),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ActiveNote *active = static_cast<ActiveNote *>(heap_caps_calloc(
        scratch->note_on_count,
        sizeof(ActiveNote),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (notes == nullptr || active == nullptr) {
        if (notes != nullptr) heap_caps_free(notes);
        if (active != nullptr) heap_caps_free(active);
        return ESP_ERR_NO_MEM;
    }

    size_t note_count = 0U;
    size_t active_count = 0U;
    size_t tempo_index = 0U;
    uint32_t tempo = 500000U;
    uint64_t segment_tick = 0U;
    uint64_t segment_usec = 0U;
    uint8_t min_note = 127U;
    uint8_t max_note = 0U;
    uint32_t max_note_duration_ms = 0U;
    uint8_t channel_program[16] = {}; // GM 默认 Program 0：Acoustic Grand Piano

    auto tick_to_absolute_usec = [&](uint64_t target_tick) -> uint64_t {
        while (tempo_index < scratch->tempo_count &&
            scratch->tempos[tempo_index].tick <= target_tick) {
            const TempoEvent &change = scratch->tempos[tempo_index++];
            if (change.tick > segment_tick) {
                const uint64_t add = ticks_to_usec(change.tick - segment_tick, tempo, division);
                if (UINT64_MAX - segment_usec < add) segment_usec = UINT64_MAX;
                else segment_usec += add;
                segment_tick = change.tick;
            }
            tempo = change.usec_per_quarter;
        }
        if (target_tick <= segment_tick) return segment_usec;
        const uint64_t add = ticks_to_usec(target_tick - segment_tick, tempo, division);
        return UINT64_MAX - segment_usec < add ? UINT64_MAX : segment_usec + add;
    };

    auto find_active = [&](uint16_t track, uint8_t channel, uint8_t note) -> size_t {
        for (size_t i = active_count; i > 0U; --i) {
            const ActiveNote &slot = active[i - 1U];
            if (slot.track == track && slot.channel == channel && slot.note == note) {
                return i - 1U;
            }
        }
        return SIZE_MAX;
    };

    uint32_t max_note_end_ms = 0U;
    auto emit_active = [&](size_t active_index, uint32_t end_ms) {
        if (active_index >= active_count || note_count >= scratch->note_on_count) return;
        const ActiveNote slot = active[active_index];
        NoteEvent &dst = notes[note_count++];
        dst.start_ms = slot.start_ms;
        dst.end_ms = end_ms > slot.start_ms ? end_ms : slot.start_ms + 1U;
        dst.note = slot.note;
        dst.velocity = slot.velocity;
        dst.channel = slot.channel;
        dst.program = slot.program;
        if (slot.note < min_note) min_note = slot.note;
        if (slot.note > max_note) max_note = slot.note;
        const uint32_t duration = dst.end_ms - dst.start_ms;
        if (duration > max_note_duration_ms) max_note_duration_ms = duration;
        if (dst.end_ms > max_note_end_ms) max_note_end_ms = dst.end_ms;
        active[active_index] = active[active_count - 1U];
        --active_count;
    };

    for (size_t i = 0U; i < scratch->event_count; ++i) {
        if ((i & 0xFFU) == 0U && !generation_current(generation)) {
            heap_caps_free(active);
            heap_caps_free(notes);
            return ESP_ERR_INVALID_STATE;
        }
        const RawEvent &event = scratch->events[i];
        const uint32_t time_ms = clamp_ms(tick_to_absolute_usec(event.tick));
        if (event.type == RawEventType::ProgramChange) {
            if (event.channel < 16U) channel_program[event.channel] = event.program;
            continue;
        }
        const size_t active_index = find_active(event.track, event.channel, event.note);
        if (event.type == RawEventType::NoteOn) {
            // 同轨同通道同音高发生重触发时先结束上一音符，避免瀑布块无限延伸。
            if (active_index != SIZE_MAX) emit_active(active_index, time_ms);
            if (active_count >= scratch->note_on_count) continue;
            ActiveNote &slot = active[active_count++];
            slot.start_ms = time_ms;
            slot.track = event.track;
            slot.note = event.note;
            slot.velocity = event.velocity;
            slot.channel = event.channel;
            slot.program = event.channel < 16U ? channel_program[event.channel] : 0U;
        } else if (event.type == RawEventType::NoteOff && active_index != SIZE_MAX) {
            emit_active(active_index, time_ms);
        }
    }

    const uint32_t timeline_end_ms = clamp_ms(tick_to_absolute_usec(scratch->max_tick));
    while (active_count > 0U) emit_active(active_count - 1U, timeline_end_ms);
    heap_caps_free(active);

    if (note_count == 0U) {
        heap_caps_free(notes);
        return ESP_ERR_NOT_FOUND;
    }
    qsort(notes, note_count, sizeof(NoteEvent), note_compare);

    uint32_t duration_ms = timeline_end_ms;
    if (max_note_end_ms > duration_ms) duration_ms = max_note_end_ms;
    out->notes = notes;
    out->note_count = note_count;
    out->duration_ms = duration_ms;
    out->max_note_duration_ms = max_note_duration_ms;
    out->division = division;
    out->track_count = track_count;
    out->min_note = min_note;
    out->max_note = max_note;
    out->format = format;
    return ESP_OK;
}

static esp_err_t parse_midi(
    const uint8_t *data,
    size_t size,
    uint32_t generation,
    Timeline *out)
{
    if (data == nullptr || out == nullptr || size < 14U) return ESP_ERR_INVALID_ARG;
    if (memcmp(data, "MThd", 4U) != 0) return ESP_ERR_INVALID_RESPONSE;
    const uint32_t header_length = read_be32(data + 4U);
    if (header_length < 6U || 8ULL + header_length > size) return ESP_ERR_INVALID_RESPONSE;

    const uint16_t format = read_be16(data + 8U);
    const uint16_t track_count = read_be16(data + 10U);
    const uint16_t division = read_be16(data + 12U);
    if (format > 1U || track_count == 0U) return ESP_ERR_NOT_SUPPORTED;
    if ((division & 0x8000U) != 0U || division == 0U) return ESP_ERR_NOT_SUPPORTED;

    ParseScratch scratch = {};
    size_t pos = 8U + static_cast<size_t>(header_length);
    esp_err_t ret = ESP_OK;
    uint16_t parsed_tracks = 0U;
    while (parsed_tracks < track_count && pos + 8U <= size) {
        if (!generation_current(generation)) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        if (memcmp(data + pos, "MTrk", 4U) != 0) {
            ret = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        const uint32_t track_length = read_be32(data + pos + 4U);
        pos += 8U;
        if (track_length > size - pos) {
            ret = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        ret = parse_track(data + pos, track_length, parsed_tracks, generation, &scratch);
        if (ret != ESP_OK) break;
        pos += track_length;
        ++parsed_tracks;
    }

    if (ret == ESP_OK && parsed_tracks != track_count) ret = ESP_ERR_INVALID_RESPONSE;
    if (ret == ESP_OK) {
        ret = build_timeline(
            &scratch,
            division,
            track_count,
            static_cast<uint8_t>(format),
            generation,
            out);
    }

    if (scratch.events != nullptr) heap_caps_free(scratch.events);
    if (scratch.tempos != nullptr) heap_caps_free(scratch.tempos);
    return ret;
}

static esp_err_t load_file(uint32_t generation, const char *path, uint8_t **out_data, size_t *out_size)
{
    if (path == nullptr || out_data == nullptr || out_size == nullptr) return ESP_ERR_INVALID_ARG;
    *out_data = nullptr;
    *out_size = 0U;

    FILE *file = nullptr;
    long file_size = 0L;
    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (!guard) return ESP_ERR_TIMEOUT;
        file = fopen(path, "rb");
        if (file == nullptr) return ESP_ERR_NOT_FOUND;
        if (fseek(file, 0L, SEEK_END) != 0) {
            fclose(file);
            return ESP_FAIL;
        }
        file_size = ftell(file);
        if (file_size <= 0L || fseek(file, 0L, SEEK_SET) != 0) {
            fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    const size_t bytes = static_cast<size_t>(file_size);
    uint8_t *data = static_cast<uint8_t *>(heap_caps_malloc(
        bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr) {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard && file != nullptr) fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0U;
    esp_err_t ret = ESP_OK;
    while (offset < bytes && generation_current(generation)) {
        const size_t wanted = (bytes - offset) < kReadChunkBytes ? (bytes - offset) : kReadChunkBytes;
        size_t got = 0U;
        {
            StorageSdLockGuard guard(kStorageLockTimeout);
            if (!guard) {
                vTaskDelay(1);
                continue;
            }
            got = fread(data + offset, 1U, wanted, file);
            if (got == 0U) {
                ret = feof(file) ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
            }
        }
        if (ret != ESP_OK) break;
        offset += got;
        taskYIELD();
    }
    if (!generation_current(generation)) ret = ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK && offset != bytes) ret = ESP_ERR_INVALID_SIZE;

    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard && file != nullptr) fclose(file);
    }

    if (ret != ESP_OK) {
        heap_caps_free(data);
        return ret;
    }
    *out_data = data;
    *out_size = bytes;
    return ESP_OK;
}

static void clear_pending_result()
{
    LoadResult pending = {};
    bool has_pending = false;
    portENTER_CRITICAL(&g_mux);
    if (g_result_pending) {
        pending = g_pending_result;
        g_pending_result = {};
        g_result_pending = false;
        has_pending = true;
    }
    portEXIT_CRITICAL(&g_mux);
    if (has_pending) release_timeline(&pending.timeline);
}

static bool publish_result(LoadResult *result)
{
    if (result == nullptr) return false;
    bool accepted = false;
    portENTER_CRITICAL(&g_mux);
    if (result->generation == g_generation && !g_result_pending) {
        g_pending_result = *result;
        g_result_pending = true;
        result->timeline = {};
        accepted = true;
    }
    portEXIT_CRITICAL(&g_mux);
    return accepted;
}

static void load_task(void *arg)
{
    TaskArgs *args = static_cast<TaskArgs *>(arg);
    if (args == nullptr || args->path == nullptr) {
        if (args != nullptr) heap_caps_free(args);
        vTaskDelete(nullptr);
        return;
    }

    LoadResult result = {};
    result.state = LoadState::Failed;
    result.generation = args->generation;
    result.result = ESP_FAIL;

    uint8_t *file_data = nullptr;
    size_t file_size = 0U;
    esp_err_t ret = load_file(args->generation, args->path, &file_data, &file_size);
    if (ret == ESP_OK && generation_current(args->generation)) {
        ret = parse_midi(file_data, file_size, args->generation, &result.timeline);
    }
    if (file_data != nullptr) heap_caps_free(file_data);

    result.result = ret;
    result.state = ret == ESP_OK ? LoadState::Ready : LoadState::Failed;
    const Timeline log_timeline = result.timeline;
    if (publish_result(&result)) {
        if (ret == ESP_OK) {
            ESP_LOGI(
                TAG,
                "MIDI解析完成：format=%u tracks=%u division=%u notes=%u range=%u-%u duration=%lums",
                static_cast<unsigned>(log_timeline.format),
                static_cast<unsigned>(log_timeline.track_count),
                static_cast<unsigned>(log_timeline.division),
                static_cast<unsigned>(log_timeline.note_count),
                static_cast<unsigned>(log_timeline.min_note),
                static_cast<unsigned>(log_timeline.max_note),
                static_cast<unsigned long>(log_timeline.duration_ms));
        } else {
            ESP_LOGW(TAG, "MIDI解析失败：path=%s ret=%s", args->path, esp_err_to_name(ret));
        }
    } else {
        release_timeline(&result.timeline);
    }

    heap_caps_free(args->path);
    heap_caps_free(args);
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t init()
{
    if (g_initialized) return ESP_OK;
    clear_pending_result();
    g_initialized = true;
    return ESP_OK;
}

esp_err_t start(const char *path)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    if (path == nullptr || path[0] == '\0') return ESP_ERR_INVALID_ARG;

    TaskArgs *args = static_cast<TaskArgs *>(heap_caps_calloc(
        1U,
        sizeof(TaskArgs),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    const size_t path_bytes = strlen(path) + 1U;
    char *path_copy = static_cast<char *>(heap_caps_malloc(
        path_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (args == nullptr || path_copy == nullptr) {
        if (args != nullptr) heap_caps_free(args);
        if (path_copy != nullptr) heap_caps_free(path_copy);
        return ESP_ERR_NO_MEM;
    }
    memcpy(path_copy, path, path_bytes);

    portENTER_CRITICAL(&g_mux);
    ++g_generation;
    if (g_generation == 0U) ++g_generation;
    args->generation = g_generation;
    portEXIT_CRITICAL(&g_mux);
    args->path = path_copy;
    clear_pending_result();

    const BaseType_t created = xTaskCreatePinnedToCore(
        load_task,
        "MidiLoadTask",
        kTaskStack,
        args,
        kTaskPriority,
        nullptr,
        kTaskCore);
    if (created != pdPASS) {
        heap_caps_free(path_copy);
        heap_caps_free(args);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void cancel()
{
    portENTER_CRITICAL(&g_mux);
    ++g_generation;
    if (g_generation == 0U) ++g_generation;
    portEXIT_CRITICAL(&g_mux);
    clear_pending_result();
}

bool take_result(LoadResult *out_result)
{
    if (out_result == nullptr || !g_initialized) return false;
    bool available = false;
    portENTER_CRITICAL(&g_mux);
    if (g_result_pending && g_pending_result.generation == g_generation) {
        *out_result = g_pending_result;
        g_pending_result = {};
        g_result_pending = false;
        available = true;
    }
    portEXIT_CRITICAL(&g_mux);
    return available;
}

void release_timeline(Timeline *timeline)
{
    if (timeline == nullptr) return;
    if (timeline->notes != nullptr) heap_caps_free(timeline->notes);
    *timeline = {};
}

} // namespace VisualMusicMidi
