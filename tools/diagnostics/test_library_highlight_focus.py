"""Run production group/focus helpers with host fixtures; not an ESP-IDF build."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    text = (ROOT / "src/ui/screens/library_view.cpp").read_text()
    signatures = [
        "static int32_t library_view_scroll_for_position(uint32_t position)",
        "static uint32_t library_view_current_group(LibraryBrowseMode mode)",
        "static bool library_view_group_contains_current_track(LibraryBrowseMode mode, uint32_t group_index)",
        "static bool library_view_focus_current_playback()\n{",
        "static void library_view_apply_playback_focus()",
        "void library_view_request_playback_focus()",
    ]
    helpers = "\n".join(function(text, sig) for sig in signatures)
    remote = (ROOT / "src/ui/screens/library_remote.inc").read_text()
    timer_line = next(line for line in remote.splitlines() if "g_playback_focus_pending.exchange(false)" in line)
    before = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
#define ESP_LOGI(...) ((void)0)
enum class LibraryDataSource { Local, Nas };
enum class PlayerMediaSource { Local, Nas };
enum class LibraryBrowseMode : uint8_t { AllTracks, Artists, Albums, Decades, GroupTracks };
struct Group { const uint32_t *track_indices = nullptr; uint32_t track_count = 0; };
using MediaArtistGroupViewV2 = Group;
using MediaAlbumGroupViewV2 = Group;
using MediaDecadeGroupViewV2 = Group;
static std::vector<std::vector<uint32_t>> members[3] = {
    {{0,2},{1,2,3}}, {{0,2},{1,3}}, {{0,1},{2,3}}
};
static LibraryDataSource g_source = LibraryDataSource::Local;
static PlayerMediaSource player_source = PlayerMediaSource::Local;
static bool ready = true, visible = true, folder = false;
static uint32_t track = 2, generation = 1, reads = 0, rendered = 0, refreshed = 0;
static std::atomic<bool> g_playback_focus_pending{false};
static constexpr uint32_t LIBRARY_ROW_STEP = 50;
static struct {
 LibraryBrowseMode mode = LibraryBrowseMode::AllTracks;
 uint32_t detail_type = 1, detail_group_index = 0;
 int32_t top_scroll_y[4] = {}, detail_scroll_y = 0;
} g_state;
static struct { bool active = false; } g_search;
bool player_state_is_ready() { return ready; }
PlayerMediaSource player_state_get_source() { return player_source; }
uint32_t player_state_get_index() { return track; }
bool library_source_is_nas() { return g_source == LibraryDataSource::Nas; }
uint32_t library_source_generation() { return generation; }
bool library_view_folder_browser_active() { return folder; }
bool library_view_is_visible() { return visible; }
void library_view_render(bool) { ++rendered; }
void library_view_refresh_virtual_rows(bool) { ++refreshed; }
uint32_t library_view_top_level_count(LibraryBrowseMode mode) {
 if(mode == LibraryBrowseMode::AllTracks)return 4;
 return members[static_cast<unsigned>(mode)-1].size();
}
bool get_group(unsigned type,uint32_t id,Group *out) {
 ++reads;
 if(id >= members[type].size())return false;
 out->track_indices = members[type][id].data(); out->track_count = members[type][id].size(); return true;
}
bool library_source_get_artist(uint32_t id,Group *out) { return get_group(0,id,out); }
bool library_source_get_album(uint32_t id,Group *out) { return get_group(1,id,out); }
bool library_source_get_decade(uint32_t id,Group *out) { return get_group(2,id,out); }
bool library_view_top_track_index(uint32_t position,uint32_t *out) { *out = position;return position < 4; }
bool library_view_get_detail_identity(void*,uint32_t *count,void*) {
 *count = members[g_state.detail_type-1][g_state.detail_group_index].size();return true;
}
bool library_view_detail_track_index(uint32_t position,uint32_t *out,void*) {
 auto &ids = members[g_state.detail_type-1][g_state.detail_group_index];
 if(position >= ids.size())return false;
 *out = ids[position];return true;
}
'''
    after = r'''
int main() {
 assert(library_view_current_group(LibraryBrowseMode::Artists) == 0);
 auto before = reads;
 for(int i=0;i<1000;++i)assert(library_view_current_group(LibraryBrowseMode::Artists) == 0);
 assert(reads == before); // Repeated focus does not rescan the catalog.
 assert(library_view_group_contains_current_track(LibraryBrowseMode::Artists,0));
 assert(library_view_group_contains_current_track(LibraryBrowseMode::Artists,1)); // Collaborator.
 assert(library_view_current_group(LibraryBrowseMode::Albums) == 0);
 assert(!library_view_group_contains_current_track(LibraryBrowseMode::Albums,1));
 assert(library_view_current_group(LibraryBrowseMode::Decades) == 1);
 track = 3;assert(library_view_current_group(LibraryBrowseMode::Albums) == 1);
 track = 2;assert(library_view_current_group(LibraryBrowseMode::Artists) == 0);
 members[0][0] = {0};++generation;
 assert(library_view_current_group(LibraryBrowseMode::Artists) == 1); // Hot-reload invalidates.
 player_source = PlayerMediaSource::Nas;
 assert(library_view_current_group(LibraryBrowseMode::Artists) == UINT32_MAX);
 assert(!library_view_group_contains_current_track(LibraryBrowseMode::Artists,1));
 g_source = LibraryDataSource::Nas;
 assert(library_view_current_group(LibraryBrowseMode::Artists) == 1); // Same ids, new source.
 ready = false;assert(library_view_current_group(LibraryBrowseMode::Artists) == UINT32_MAX);ready = true;
 track = UINT32_MAX;assert(library_view_current_group(LibraryBrowseMode::Artists) == UINT32_MAX);
 track = 3;g_state.mode = LibraryBrowseMode::AllTracks;
 assert(library_view_focus_current_playback() && g_state.top_scroll_y[0] == 50);
 for(auto mode : {LibraryBrowseMode::Artists,LibraryBrowseMode::Albums,LibraryBrowseMode::Decades}) {
  g_state.mode = mode;assert(library_view_focus_current_playback());
 }
 g_state.mode = LibraryBrowseMode::GroupTracks;g_state.detail_type = 1;g_state.detail_group_index = 1;
 assert(library_view_focus_current_playback());
 g_state.detail_group_index = 0;assert(!library_view_focus_current_playback());
 g_search.active = true;assert(!library_view_focus_current_playback());g_search.active = false;
 folder = true;assert(!library_view_focus_current_playback());folder = false;
 g_state.mode = LibraryBrowseMode::AllTracks;
 for(int i=0;i<10000;++i)library_view_request_playback_focus();
 assert(rendered == 0 && refreshed == 0 && g_playback_focus_pending.load());
 ui_tick();assert(rendered == 1 && !g_playback_focus_pending.load());
 ui_tick();assert(rendered == 1); // Latest track handled once by UI, no queued backlog.
 visible = false;library_view_request_playback_focus();ui_tick();assert(rendered == 1);
 std::cout << "PASS: actual firmware group cache, collaborators, generation/source invalidation, focus, search/folder guards, coalesced UI timer\n";
}
'''
    with tempfile.TemporaryDirectory() as folder:
        source, binary = Path(folder) / "test.cpp", Path(folder) / "test"
        source.write_text(before + helpers + "\nvoid ui_tick() {" + timer_line + "}\n" + after)
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
