#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "audio_types.h"
#include "midi_synth.h"
#include "nsf_synth.h"

// 启动唯一的 AudioTask。运行期所有 DAC/I2S 控制都必须由该任务执行。
esp_err_t audio_service_start();

// 判断 AudioTask 是否已经完成硬件初始化。
bool audio_service_is_ready();

// 获取 AudioTask 发布的只读状态快照。
bool audio_service_get_snapshot(AudioStateSnapshot *out_snapshot);

// R.36.2.2：获取最近一次真实播放故障的 RAM 快照。
// 快照会保留到下一次故障或重启，便于偶发停播后再接串口读取。
bool audio_service_get_last_fault(AudioFaultSnapshot *out_snapshot);
void audio_service_log_last_fault();

// P1.5.2R.3：获取低优先级 SpectrumFFT 任务发布的 16 路真实频率 band。
// UI 只能读取快照，不能直接访问 PCM/decoder/I2S。
bool audio_service_get_spectrum_snapshot(AudioSpectrumSnapshot *out_snapshot);

// 频谱页面显示/隐藏时启停旁路分析。这里只控制观察者，不改变音频 pipeline。
void audio_service_set_spectrum_enabled(bool enabled);

// 请求播放指定曲目。路径和技术索引都会复制到请求对象，避免跨任务悬空指针。
// Stage 10.1 只传递/校验索引快照，decoder 仍以实际文件解析结果作为播放真值。
bool audio_service_play_track(
    uint32_t track_index,
    const char *path,
    MediaFormat format,
    const MediaTechnicalInfo *technical_info,
    bool wait = false
);

// 停止、暂停和恢复均通过 AudioTask 串行执行。
bool audio_service_stop(bool wait = true);
bool audio_service_pause(bool wait = true);
bool audio_service_resume(bool wait = true);

// R.40.4.1：AVI 内 MP3 临时接管 AudioTask 的 PCM/I2S/CS43131 输出。
// 调用前 Video Exclusive 已暂停当前 Music；Video MP3 stop 后会恢复 Paused Music 硬件，
// 真正的 Music resume 仍由 Video 生命周期决定。Video/Extractor 从不直接操作 I2S/DAC。
bool audio_service_video_mp3_start(
    uint32_t sample_rate_hz,
    uint8_t channels,
    uint8_t bits_per_sample,
    bool wait = true
);

// R.40.4.3：A/V Start Barrier。prepare 只完成 MP3 decoder + I2S/DAC 预备并保持静音零PCM，
// 不消费/提交真实 PCM；Dedicated Presenter 在首帧窗口和黑场准备完成后调用 release_start，
// 让首个真实 PCM 与首个视频帧从同一启动边界并行前进。
bool audio_service_video_mp3_prepare(
    uint32_t sample_rate_hz,
    uint8_t channels,
    uint8_t bits_per_sample,
    bool wait = true
);
bool audio_service_video_mp3_release_start(bool wait = true);
bool audio_service_video_mp3_stop(bool wait = true);

// R.40.4.2：Video Presenter / PreDecode 只读的 AVI MP3 PCM 主时钟快照。
// position_us 来自“真实已成功提交到 I2S DMA”的 PCM sample 数；Video 永远只读，
// 不得通过该接口控制 AudioTask / I2S / DAC。
struct AudioVideoClockSnapshot
{
    bool active = false;
    bool eof = false;
    bool start_released = true;
    uint32_t revision = 0U;
    uint32_t sample_rate_hz = 0U;
    uint64_t submitted_frames = 0ULL;
    uint64_t decoder_frames = 0ULL;
    uint64_t position_us = 0ULL;
};
bool audio_service_video_mp3_get_clock(AudioVideoClockSnapshot *out_snapshot);

// VM07：电子音流 MIDI 使用 AudioTask 内置轻量 GM-Lite Synth 直接产出 48kHz stereo PCM。
// notes 会在提交时复制到 AudioTask 自有 PSRAM，调用返回后原数组可立即释放/复用。
bool audio_service_midi_start(
    const MidiSynthNoteEvent *notes,
    size_t note_count,
    uint32_t duration_ms,
    bool wait = true);
bool audio_service_midi_pause(bool wait = true);
bool audio_service_midi_resume(bool wait = true);
bool audio_service_midi_restart(bool wait = true);
// restore_music_hardware=false 用于同一电子音流 APP 内切换下一首，避免中途恢复 Music DAC 再立即关闭。
bool audio_service_midi_stop(bool restore_music_hardware = true, bool wait = true);

struct AudioMidiClockSnapshot
{
    bool active = false;
    bool paused = false;
    bool eof = false;
    uint32_t revision = 0U;
    uint32_t sample_rate_hz = 0U;
    uint64_t submitted_frames = 0ULL;
    uint64_t position_ms = 0ULL;
    uint32_t duration_ms = 0U;
};
bool audio_service_midi_get_clock(AudioMidiClockSnapshot *out_snapshot);

// VM10：传统 NESM NSF 由 AudioTask 内置 6502/2A03 Core 产出 48kHz stereo PCM。
// prg 会在提交时复制到 AudioTask 自有 PSRAM；第一阶段只支持 NTSC + 基础五通道。
bool audio_service_nsf_start(
    const uint8_t *prg,
    size_t prg_size,
    const NsfSynthConfig *config,
    bool wait = true);
bool audio_service_nsf_pause(bool wait = true);
bool audio_service_nsf_resume(bool wait = true);
bool audio_service_nsf_set_track(uint8_t track, bool wait = true);
// restore_music_hardware=false 用于同一 NSF 内切 Subsong，正常列表/退出路径应传 true。
bool audio_service_nsf_stop(bool restore_music_hardware = true, bool wait = true);

struct AudioNsfClockSnapshot
{
    bool active = false;
    bool paused = false;
    bool eof = false;
    bool failed = false;
    uint32_t revision = 0U;
    uint32_t sample_rate_hz = 0U;
    uint64_t submitted_frames = 0ULL;
    uint64_t position_ms = 0ULL;
    uint64_t duration_ms = 0ULL; // 两轮结构一致可提前显示时长；自动结束仍等待后台可靠确认，无Loop/未知时为0
    uint8_t track = 0U;       // 0-based
    uint8_t track_count = 0U;
};
bool audio_service_nsf_get_clock(AudioNsfClockSnapshot *out_snapshot);

enum class AudioNsfVisualVoice : uint8_t
{
    Pulse1 = 0U,
    Pulse2,
    Triangle,
    Noise,
    Dmc,
};

struct AudioNsfVisualEvent
{
    uint32_t start_ms = 0U;
    uint32_t end_ms = 0U;
    uint8_t note = 0U;   // Pulse/Triangle 使用MIDI音高；Noise/DMC忽略
    uint8_t level = 0U;  // 0~127
    AudioNsfVisualVoice voice = AudioNsfVisualVoice::Pulse1;
    uint8_t reserved = 0U;
};
static_assert(sizeof(AudioNsfVisualEvent) == 12U, "AudioNsfVisualEvent must remain compact");

// 复制当前 NSF 真实播放路在给定时间窗内的瀑布事件；用于“正在播/刚播过”的严格同步部分。
size_t audio_service_nsf_copy_visual_events(
    uint8_t track,
    uint32_t window_start_ms,
    uint32_t window_end_ms,
    AudioNsfVisualEvent *out_events,
    size_t capacity);

// 复制独立 NsfPreviewTask 预读的未来音符。事件使用曲目绝对毫秒时间戳；UI 必须仍以真实 I2S position_ms 对齐，
// 预读任务只维持约4~5秒领先，不参与 Loop/时长分析，也不得作为当前播放时间。
size_t audio_service_nsf_copy_lookahead_events(
    uint8_t track,
    uint32_t window_start_ms,
    uint32_t window_end_ms,
    AudioNsfVisualEvent *out_events,
    size_t capacity);

// Stage 11.x：对当前选中 Track 发起 Seek。Play/Seek 共用 latest-intent 单槽；
// 路径/技术索引仍复制进请求，AudioTask 会核对 track + playback_revision，并丢弃被更新意图覆盖的旧请求。
bool audio_service_seek_track(
    uint32_t track_index,
    const char *path,
    MediaFormat format,
    const MediaTechnicalInfo *technical_info,
    uint64_t target_ms,
    bool wait = false
);

// 用户音量/静音同样只能通过 AudioTask 命令队列改变。
// percent 范围 0~100；R.13 默认 50%≈-18dB，保持接近旧版 80%=-20dB 的启动实际响度。
bool audio_service_set_volume(uint8_t percent, bool wait = false);
bool audio_service_set_mute(bool mute, bool wait = false);

// 当前播放世代。播放新曲或停止时递增，用于后续取消过期异步操作。
uint32_t audio_service_playback_revision();
