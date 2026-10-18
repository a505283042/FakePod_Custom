"""Host regression for the production NAS post-switch binding helper (requires g++).

Runs the actual helper extracted from library_view.cpp with simulated service states.
This does not replace an ESP-IDF build or on-device UI/audio verification.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = source.index('static void ' + name + '()')
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    helper = function((ROOT / 'src/ui/screens/library_view.cpp').read_text(encoding='utf-8'),
                      'library_view_bind_new_nas_playback')
    before = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
enum class DeviceMusicLibrarySource { Local, Nas };
enum class PlayerMediaSource { Local, Nas };
enum class AudioPlaybackState { Ready, Stopped, Error, Finished, Playing, Paused, Opening, Seeking };
struct AudioStateSnapshot { AudioPlaybackState state = AudioPlaybackState::Stopped; };
static bool g_nas_server_bind_pending, loaded, selected_ready, snapshot_ready, bind_ok;
static uint32_t tracks;
static DeviceMusicLibrarySource browse;
static PlayerMediaSource selected;
static AudioPlaybackState audio;
static unsigned selections;
static constexpr const char *TAG = "test";
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
DeviceMusicLibrarySource device_settings_music_library_source() { return browse; }
bool nas_library_source_ready() { return loaded; }
uint32_t nas_library_source_track_count() { return tracks; }
PlayerMediaSource player_state_get_source() { return selected; }
bool player_state_is_ready() { return selected_ready; }
bool audio_service_get_snapshot(AudioStateSnapshot *out) { out->state = audio; return snapshot_ready; }
bool player_control_select_nas_all_tracks(unsigned position) {
    assert(position == 0); ++selections;
    if (bind_ok) { selected = PlayerMediaSource::Nas; selected_ready = true; }
    return bind_ok;
}
void reset() {
    g_nas_server_bind_pending = true; loaded = true; selected_ready = true;
    snapshot_ready = true; bind_ok = true; tracks = 4;
    browse = DeviceMusicLibrarySource::Nas; selected = PlayerMediaSource::Local;
    audio = AudioPlaybackState::Stopped; selections = 0;
}
'''
    after = r'''
int main() {
    reset(); library_view_bind_new_nas_playback();
    assert(selected == PlayerMediaSource::Nas && selections == 1 && !g_nas_server_bind_pending);
    assert(audio == AudioPlaybackState::Stopped); // Selection must not start playback.
    library_view_bind_new_nas_playback(); assert(selections == 1); // One-shot.
    reset(); g_nas_server_bind_pending = false; library_view_bind_new_nas_playback();
    assert(selected == PlayerMediaSource::Local && selections == 0); // Ordinary browsing/reload.
    reset(); loaded = false; library_view_bind_new_nas_playback();
    assert(selections == 0 && g_nas_server_bind_pending);
    loaded = true; library_view_bind_new_nas_playback(); assert(selections == 1); // Retry load.
    reset(); tracks = 0; library_view_bind_new_nas_playback(); assert(selections == 0);
    reset(); browse = DeviceMusicLibrarySource::Local; library_view_bind_new_nas_playback();
    assert(selections == 0 && !g_nas_server_bind_pending);
    reset(); selected = PlayerMediaSource::Nas; library_view_bind_new_nas_playback();
    assert(selections == 0 && !g_nas_server_bind_pending); // Keep deliberate NAS selection.
    for (auto state : {AudioPlaybackState::Playing, AudioPlaybackState::Paused,
                       AudioPlaybackState::Opening, AudioPlaybackState::Seeking}) {
        reset(); audio = state; library_view_bind_new_nas_playback();
        assert(selections == 0 && !g_nas_server_bind_pending); // Keep new user playback intent.
    }
    for (auto state : {AudioPlaybackState::Ready, AudioPlaybackState::Stopped,
                       AudioPlaybackState::Error, AudioPlaybackState::Finished}) {
        reset(); audio = state; library_view_bind_new_nas_playback(); assert(selections == 1);
    }
    reset(); snapshot_ready = false; library_view_bind_new_nas_playback();
    assert(selections == 0 && g_nas_server_bind_pending);
    reset(); bind_ok = false; library_view_bind_new_nas_playback();
    assert(selected == PlayerMediaSource::Local && g_nas_server_bind_pending);
    bind_ok = true; library_view_bind_new_nas_playback(); assert(selected == PlayerMediaSource::Nas);
    std::cout << "NAS post-switch binding state regression PASS\n";
}
'''
    with tempfile.TemporaryDirectory() as folder:
        source = Path(folder) / 'check.cpp'
        binary = Path(folder) / 'check'
        source.write_text(before + helper + after, encoding='utf-8')
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
