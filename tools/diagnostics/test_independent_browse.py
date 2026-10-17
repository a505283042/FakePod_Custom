"""Host execution of production independent playback queue; not an ESP-IDF build."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(s,sig):
 a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
def main():
 state=(ROOT/'src/player/player_state.cpp').read_text()
 header=r'''
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <cassert>
#include <string>
#include <vector>
#include <initializer_list>
#define MALLOC_CAP_SPIRAM 1
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define MALLOC_CAP_8BIT 2
constexpr size_t PLAYER_FOLDER_PATH_MAX=384;
enum class PlayerMediaSource:uint8_t{Local,Nas};
enum class PlayerListType:uint8_t{AllTracks,Artist,Album,Decade};
enum class PlayerFolderScope:uint8_t{All,Level1,Level2};
struct PlayerListSnapshot{bool ready=true;PlayerListType type=PlayerListType::AllTracks;uint32_t catalog_generation=1,group_index=0,group_id=0,position=0,track_count=4,track_index=0;};
struct PlayerFolderQueueSnapshot{bool ready=false;PlayerFolderScope preferred_scope=PlayerFolderScope::All,effective_scope=PlayerFolderScope::All;uint32_t catalog_generation=0,context_id=0,position=0,track_count=0,track_index=UINT32_MAX;bool current_in_queue=false;};
struct MediaArtistGroupViewV2{const uint32_t*track_indices;uint32_t track_count;};
using MediaAlbumGroupViewV2=MediaArtistGroupViewV2;using MediaDecadeGroupViewV2=MediaArtistGroupViewV2;
enum class MediaFormat{MP3,FLAC,Other};
struct FakeRow{MediaFormat format=MediaFormat::FLAC;};static FakeRow row;
struct MediaTrackViewV2{const char*path;FakeRow*row=&::row;};
static PlayerMediaSource g_source=PlayerMediaSource::Local;
static bool g_ready=true;
bool player_state_is_ready(){return true;}
static uint32_t generation=1;
static int allocations=0;
void*heap_caps_malloc(size_t n,int){void*p=malloc(n);if(p)allocations++;return p;}
void heap_caps_free(void*p){if(p)allocations--;free(p);}
uint32_t media_catalog_v2_generation(){return generation;}
uint32_t nas_library_source_generation(){return generation;}
uint32_t media_library_get_count(){return 4;}
uint32_t nas_library_source_track_count(){return 4;}
static const char* local_paths[]={"/sdcard/MUSIC/A/one.flac","/sdcard/MUSIC/A/B/two.flac","/sdcard/MUSIC/A/B/C/three.flac","/sdcard/MUSIC/Z/four.flac"};
static const char* nas_paths[]={"/A/one.flac","/A/B/two.flac","/A/B/C/three.flac","/Z/four.flac"};
bool media_catalog_v2_get_track_view(uint32_t i,MediaTrackViewV2*v){if(i>=4)return false;v->path=local_paths[i];return true;}
bool nas_library_source_get_track_view(uint32_t i,MediaTrackViewV2*v){if(i>=4)return false;v->path=nas_paths[i];return true;}
static uint32_t members[]={0,2};
bool fixture_group(uint32_t i,MediaArtistGroupViewV2*v){if(i)return false;v->track_indices=members;v->track_count=2;return true;}
#define media_groups_v2_get_artist fixture_group
#define media_groups_v2_get_album fixture_group
#define media_groups_v2_get_decade fixture_group
#define nas_library_source_get_artist fixture_group
#define nas_library_source_get_album fixture_group
#define nas_library_source_get_decade fixture_group
static PlayerListSnapshot native;
static PlayerFolderScope device_scope=PlayerFolderScope::Level2;
static std::string device_path="/sdcard/MUSIC/Z/";
static int device_category=3,device_scroll=456;
bool bind(PlayerListType t,uint32_t g,size_t p){native.type=t;native.group_index=g;native.position=p;native.track_count=t==PlayerListType::AllTracks?4:2;native.track_index=t==PlayerListType::AllTracks?p:members[p];return true;}
bool nas_bind(PlayerListType t,uint32_t g,size_t p){return bind(t,g,p);}
bool player_playlist_bind_all(size_t p){return bind(PlayerListType::AllTracks,0,p);}
bool player_playlist_bind_artist(uint32_t g,size_t p){return bind(PlayerListType::Artist,g,p);}
bool player_playlist_bind_album(uint32_t g,size_t p){return bind(PlayerListType::Album,g,p);}
bool player_playlist_bind_decade(uint32_t g,size_t p){return bind(PlayerListType::Decade,g,p);}
bool player_state_select(bool b,const char*){return b;}
bool player_state_get_list_snapshot(PlayerListSnapshot*s){*s=native;return true;}
bool player_state_select_position(size_t p){return bind(native.type,native.group_index,p);}
bool nas_get_folder_queue(PlayerFolderQueueSnapshot*){return false;}
bool player_playlist_get_folder_queue_snapshot(PlayerFolderQueueSnapshot*){return false;}
bool nas_track_at_list_position(size_t p,size_t*t){*t=p;return true;}
bool player_playlist_get_folder_queue_track_index_at_position(size_t p,size_t*t){*t=p;return true;}
bool nas_select_folder_position(size_t){return false;}
bool player_playlist_select_folder_queue_position(size_t){return false;}
PlayerFolderScope nas_library_source_folder_scope(){return device_scope;}
PlayerFolderScope player_playlist_get_folder_scope(){return device_scope;}
bool nas_library_source_scoped_track_at(uint32_t,uint32_t*){return false;}
static int legacy_moves=0;
void player_state_log_current(const char*){}
bool player_playlist_folder_next(){legacy_moves++;return bind(PlayerListType::AllTracks,0,3);}
bool player_playlist_folder_previous(){legacy_moves++;return bind(PlayerListType::AllTracks,0,3);}
PlayerMediaSource player_state_get_source(){return g_source;}

'''
 body=(ROOT/'src/player/player_remote_queue.inc').read_text()+(ROOT/'src/player/player_phone_select.inc').read_text()
 for sig in ['PlayerFolderScope player_state_get_folder_scope()','bool player_state_get_folder_queue_snapshot(PlayerFolderQueueSnapshot *out_snapshot)','bool player_state_get_folder_queue_track_index_at_position(size_t position, size_t *out_track_index)','bool player_state_select_folder_queue_position(size_t position)','static bool state_move(int direction)','bool player_state_previous()','bool player_state_next()']:
  body+=function(state,sig)+'\n'
 header+=r'''
using TickType_t=uint32_t;
#define pdMS_TO_TICKS(x) (x)
#define taskENTER_CRITICAL(x) ((void)0)
#define taskEXIT_CRITICAL(x) ((void)0)
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define pdPASS 1
enum class DeviceMusicListScope{All};
enum class DeviceMusicLibrarySource{Local,Nas};
static DeviceMusicLibrarySource logical_source=DeviceMusicLibrarySource::Nas;
DeviceMusicLibrarySource device_settings_music_library_source(){return logical_source;}
int device_settings_set_music_library_source(DeviceMusicLibrarySource source){logical_source=source;return ESP_OK;}
void library_view_on_library_source_changed(DeviceMusicLibrarySource){}
static uint32_t g_nas_remote_retry_due=0;static bool g_nas_load_running=false,g_nas_load_remote_only=false,g_nas_load_show=false,g_nas_index_reload_active=false;
static DeviceMusicListScope g_nas_load_scope;static int g_nas_load_result=0;
using TaskHandle_t=void*;
constexpr int NAS_LIBRARY_LOAD_TASK_STACK_BYTES=6144,NAS_LIBRARY_LOAD_TASK_PRIORITY=1,NAS_LIBRARY_LOAD_TASK_CORE=1,NAS_LIBRARY_LOAD_TASK_STACK_CAPS=1;
void library_view_nas_load_task(void*){}
template<class...T>int xTaskCreatePinnedToCoreWithCaps(T...){return 1;}
uint32_t xTaskGetTickCount(){return 100;}
bool nas_library_source_ready(){return true;}
bool media_catalog_v2_ready(){return true;}
bool nas_catalog_service_is_server_switching(){return false;}
bool usb_storage_service_blocks_normal_runtime(){return false;}
uint32_t nas_library_source_artist_count(){return 1;}uint32_t nas_library_source_album_count(){return 1;}uint32_t nas_library_source_decade_count(){return 1;}
uint32_t media_groups_v2_artist_count(){return 1;}uint32_t media_groups_v2_album_count(){return 1;}uint32_t media_groups_v2_decade_count(){return 1;}
struct Catalog{uint32_t source_crc32=99;};static Catalog cat;Catalog*media_catalog_v2_current(){return &cat;}
static uint8_t reply[2048];static uint8_t*lr_reply=reply;constexpr size_t LR_CAP=2048;
static uint8_t lr_result=0;static uint16_t lr_size=0,lr_sent=0;static uint32_t lr_crc=0;
struct Scratch{char path[384];};static Scratch scratch;static Scratch*lr_scratch=&scratch;
bool lr_music(){return true;}
static bool audio_nas=false;
enum class AudioPlaybackSource{Local,NasHttp};
enum class AudioPlaybackState{Playing,Paused};
struct AudioStateSnapshot{bool ready=true;uint32_t track_index=3,playback_revision=0;uint64_t position_ms=0;AudioPlaybackState state=AudioPlaybackState::Playing;AudioPlaybackSource source=AudioPlaybackSource::Local;};
bool audio_service_get_snapshot(AudioStateSnapshot*s){s->source=audio_nas?AudioPlaybackSource::NasHttp:AudioPlaybackSource::Local;return true;}
bool video_app_remote_control_active(){return false;}bool visual_music_app_remote_control_active(){return false;}
bool nas_library_source_folder_selection_available(PlayerFolderScope,const char*,void*){return true;}
bool player_playlist_folder_selection_available(PlayerFolderScope,const char*,void*){return true;}
bool player_state_select_phone_queue(PlayerMediaSource,PlayerListType,uint32_t,uint32_t,PlayerFolderScope,const char*);
using esp_err_t=int;
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static bool control_locked=false,lock_allowed=true;static int checkpoints=0,checkpoint_result=0;
bool player_control_lock(uint32_t){if(!lock_allowed)return false;assert(!control_locked);control_locked=true;return true;}
void player_control_unlock(){assert(control_locked);control_locked=false;}
int persistent_state_checkpoint_local(){assert(control_locked&&g_source==PlayerMediaSource::Local);checkpoints++;return checkpoint_result;}
const char*esp_err_to_name(int){return "stub";}

bool player_control_play_current(){return true;}
'''
 remote=(ROOT/'src/ui/screens/library_remote.inc').read_text()
 body+=function(remote,'static uint32_t lr_u32(')+function(remote,'static void lr_put(')+function(remote,'static uint32_t lr_crc32(')
 left=remote.index('struct LrWriter {');right=remote.index('static bool lr_available()',left)
 body+=remote[left:right]
 body+='bool lr_record(uint32_t,uint32_t i,LrWriter&w,bool,bool nas){w.u32(i);w.u32(nas?1:0);return true;}\n'
 body+=function((ROOT/'src/player/player_control.cpp').read_text(),'bool player_control_select_phone_queue(')
 body+=(ROOT/'src/ui/screens/library_remote_independent.inc').read_text()
 body+=r'''
// Run the actual transport entry points against the production captured queues.
enum class PlayerLoopMode{Sequential,ListRepeat,SingleRepeat,Shuffle};
static PlayerLoopMode loop=PlayerLoopMode::Sequential;
static uint32_t g_last_finished_playback_revision=0;static int g_track_error_skip_count=0,play_calls=0;
PlayerLoopMode player_transport_get_loop_mode(){return loop;}
bool player_transport_play_current_internal(const char*){play_calls++;return true;}
void player_transport_shuffle_ensure_context(const PlayerFolderQueueSnapshot&){}
uint32_t player_transport_shuffle_reserve_position(const PlayerFolderQueueSnapshot&q){return (q.position+1)%q.track_count;}
void player_transport_shuffle_clear_reservation(){}
void player_transport_shuffle_push(uint32_t){}
bool player_transport_shuffle_pop(uint32_t*){return false;}
size_t player_state_get_index(){return native.track_index;}
bool player_transport_seek_ms(uint32_t){return true;}
'''
 transport=(ROOT/'src/player/player_transport.cpp').read_text()
 for sig in ['static bool player_transport_shuffle_next(const char *reason)','bool player_transport_previous()','bool player_transport_next()','void player_transport_handle_finished_event(const AudioStateSnapshot &audio)']:
  body+=function(transport,sig)+'\n'

 tests=r'''
int main(){
 PlayerFolderQueueSnapshot q;
 uint32_t revision=0;
 for(auto source:{PlayerMediaSource::Local,PlayerMediaSource::Nas}){
  const bool nas=source==PlayerMediaSource::Nas;audio_nas=nas;
  const char*folder=nas?"/A/":"/sdcard/MUSIC/A/";
  assert(player_state_select_phone_queue(source,PlayerListType::AllTracks,UINT32_MAX,0,PlayerFolderScope::Level1,folder));
  loop=PlayerLoopMode::Sequential;assert(player_transport_next());
  assert(native.track_index==1&&player_state_get_folder_queue_snapshot(&q)&&q.track_index==1&&q.position==1);
  assert(player_transport_previous());assert(native.track_index==0);
  auto eof=[&]{AudioStateSnapshot a;a.source=nas?AudioPlaybackSource::NasHttp:AudioPlaybackSource::Local;a.track_index=native.track_index;a.playback_revision=++revision;player_transport_handle_finished_event(a);};
  eof();assert(native.track_index==1);eof();assert(native.track_index==2);
  int plays=play_calls;eof();assert(native.track_index==2&&play_calls==plays); // Sequential stops at end.
  loop=PlayerLoopMode::ListRepeat;eof();assert(native.track_index==0);
  loop=PlayerLoopMode::SingleRepeat;plays=play_calls;eof();assert(native.track_index==0&&play_calls==plays+1);
  loop=PlayerLoopMode::Shuffle;assert(player_transport_next());assert(native.track_index==1);
  eof();assert(native.track_index==2);
  AudioStateSnapshot stale;stale.source=nas?AudioPlaybackSource::NasHttp:AudioPlaybackSource::Local;stale.track_index=0;stale.playback_revision=++revision;
  plays=play_calls;player_transport_handle_finished_event(stale);assert(native.track_index==2&&play_calls==plays);
  assert(legacy_moves==0&&device_scope==PlayerFolderScope::Level2&&device_path=="/sdcard/MUSIC/Z/");
 }
 // Old catalog IDs must not fall through to the device's directory queue.
 assert(player_state_select_phone_queue(PlayerMediaSource::Local,PlayerListType::AllTracks,UINT32_MAX,0,PlayerFolderScope::Level1,"/sdcard/MUSIC/A/"));
 generation++;assert(!player_state_next()&&!player_state_previous()&&legacy_moves==0);phone_queue_clear();
 assert(player_state_next()&&legacy_moves==1); // Preserve native behavior without a phone queue.
 legacy_moves=0;
 // The real phone selection wrapper checkpoints only Local -> NAS, under its lock.
 g_source=PlayerMediaSource::Local;
 assert(player_control_select_phone_queue(1,0,UINT32_MAX,0,0,"")&&checkpoints==1&&!control_locked);
 assert(player_control_select_phone_queue(1,0,UINT32_MAX,1,0,"")&&checkpoints==1);
 assert(player_control_select_phone_queue(0,0,UINT32_MAX,0,0,"")&&checkpoints==1);
 lock_allowed=false;assert(!player_control_select_phone_queue(1,0,UINT32_MAX,0,0,"")&&checkpoints==1);lock_allowed=true;
 assert(!player_control_select_phone_queue(2,0,UINT32_MAX,0,0,"")&&checkpoints==1);
 checkpoint_result=1;assert(player_control_select_phone_queue(1,0,UINT32_MAX,0,0,"")&&checkpoints==2&&!control_locked);checkpoint_result=0;
 phone_queue_clear();g_source=PlayerMediaSource::Local;audio_nas=false;
 puts("PASS: actual manual next/previous + natural EOF, Local/NAS captured queues, sequential/list/single/shuffle dispatch, stale EOF, invalid generation, native fallback and NAS local checkpoint");

 assert(player_state_select_phone_queue(PlayerMediaSource::Local,PlayerListType::Album,0,2,PlayerFolderScope::All,""));
 assert(player_state_get_folder_queue_snapshot(&q)&&q.track_count==2&&q.position==1&&q.track_index==2);
 uint32_t context=q.context_id;size_t id;
 assert(player_state_select_folder_queue_position(0));assert(native.track_index==0);
 assert(player_state_get_folder_queue_track_index_at_position(1,&id)&&id==2);
 assert(device_scope==PlayerFolderScope::Level2&&device_category==3&&device_scroll==456);
 assert(device_path=="/sdcard/MUSIC/Z/");
 assert(!player_state_select_phone_queue(PlayerMediaSource::Local,PlayerListType::Album,0,1,PlayerFolderScope::All,""));
 assert(allocations==1); // Failed membership must retain old playback queue.
 assert(player_state_select_phone_queue(PlayerMediaSource::Local,PlayerListType::AllTracks,UINT32_MAX,1,PlayerFolderScope::Level2,"/sdcard/MUSIC/A/B/"));
 assert(player_state_get_folder_queue_snapshot(&q)&&q.track_count==1&&q.track_index==1&&q.context_id!=context);
 assert(player_state_select_phone_queue(PlayerMediaSource::Nas,PlayerListType::AllTracks,UINT32_MAX,2,PlayerFolderScope::Level1,"/A/"));
 assert(player_state_get_folder_queue_snapshot(&q)&&q.track_count==3&&q.position==2);
 assert(player_state_select_folder_queue_position(1)&&native.track_index==1);
 assert(allocations==1);generation++;assert(!player_state_get_folder_queue_snapshot(&q));assert(!player_state_select_folder_queue_position(0));
 phone_queue_clear();assert(allocations==0);
 uint8_t request[32]={0x30,1,0x80,0};
 const auto category=device_category;const auto scroll=device_scroll;
 library_remote_process_independent(request);
 assert(lr_result==0&&lr_size==52&&lr_u32(reply+20)==3); // Real audio track, not selected track.
 assert(device_category==category&&device_scroll==scroll&&device_scope==PlayerFolderScope::Level2);
 request[3]=255;audio_nas=true;library_remote_process_independent(request);
 assert(lr_result==0&&reply[0]==1&&lr_u32(reply+24)==1); // Logical source is independent of audio.
 assert(device_category==category&&device_scroll==scroll);
 logical_source=DeviceMusicLibrarySource::Local;library_remote_process_independent(request);
 assert(lr_result==0&&reply[0]==0&&lr_u32(reply+24)==1); // Local browser while NAS still plays.
 request[2]=0x89;request[3]=1;library_remote_process_independent(request);
 assert(lr_result==0&&logical_source==DeviceMusicLibrarySource::Nas);
 assert(device_category==category&&device_scroll==scroll);
 request[3]=0;audio_nas=false;
 request[2]=0x81;lr_put(request+8,generation);lr_put(request+28,cat.source_crc32);
 library_remote_process_independent(request);assert(lr_result==0&&lr_size>61&&reply[60]==1);
 request[2]=0x88;lr_put(request+16,2);lr_put(request+20,2);lr_put(request+24,0);
 lr_put(request+28,999);library_remote_process_independent(request);assert(lr_result==2&&allocations==0);
 lr_put(request+28,cat.source_crc32);library_remote_process_independent(request);assert(lr_result==0&&allocations==1);
 assert(device_category==category&&device_scroll==scroll&&device_path=="/sdcard/MUSIC/Z/");
 request[2]=0x83;library_remote_process_independent(request);assert(lr_result==3); // No remote classification op.
 phone_queue_clear();assert(allocations==0);
 puts("PASS: actual independent queues, group membership validation, Local/NAS L1/L2 scope, next selection, browse unchanged, stale generation, allocation lifecycle");
}
'''
 with tempfile.TemporaryDirectory() as d:
  p=Path(d)/'test.cpp';p.write_text(header+body+tests)
  subprocess.run(['g++','-std=c++17','-Wall','-Wextra',str(p),'-o',str(Path(d)/'test')],check=True)
  subprocess.run([str(Path(d)/'test')],check=True)
if __name__=='__main__':main()
