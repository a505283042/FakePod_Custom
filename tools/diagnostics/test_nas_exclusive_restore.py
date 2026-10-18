"""Host execution of production restore handlers/lifecycle; HTTP/DAC/FreeRTOS use stand-ins."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(s,sig):
 a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
def main():
 audio=(ROOT/'src/audio/audio_service.cpp').read_text()
 nsf=(ROOT/'src/app/visual_music_app.cpp').read_text();video=(ROOT/'src/app/video_app.cpp').read_text()
 pipeline=function(audio,'static esp_err_t audio_task_start_pcm_pipeline(')
 policy='    const AudioRequest *nas_abort_request = check_transport_intent ? request : nullptr;'
 assert policy in pipeline
 assert 'audio_task_nas_open_should_abort, request' not in pipeline
 assert 'audio_task_nas_open_should_abort(request)' not in pipeline
 assert 'audio_task_nas_open_should_abort, nas_abort_request' in pipeline
 for app in [nsf,video]:
  assert 'player_state_get_technical_info(&technical)' in app
 header=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using esp_err_t=int;using TickType_t=uint32_t;
constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=1,ESP_ERR_NOT_SUPPORTED=2,ESP_FAIL=3,ESP_ERR_TIMEOUT=4;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
enum class AudioCommandType{Play,Seek,MusicDeepRestore};
enum class AudioPlaybackSource{Local,NasHttp};
enum class AudioMusicSuspendOwner{Nsf,Video};
enum class MediaFormat{Unknown,MP3,FLAC};
enum class PcmDecoderType{None,Mp3,Flac};
enum class PcmSeekMethod{Seek,RestartFromBeginning};
enum class AudioPlaybackState{Paused,Playing,Error};
enum class AudioFailureScope{System};
struct MediaTechnicalInfo{uint32_t flags=0;};struct PcmSeekResult{PcmSeekMethod method=PcmSeekMethod::Seek;};
struct AudioRequest{AudioCommandType type=AudioCommandType::Play;uint32_t request_id=1,transport_intent_revision=0,track_index=4,expected_playback_revision=9;AudioMusicSuspendOwner music_suspend_owner=AudioMusicSuspendOwner::Nsf;bool music_restore_async=false,music_restore_resume=false,has_technical_info=false;MediaTechnicalInfo technical_info;AudioPlaybackSource source=AudioPlaybackSource::NasHttp;MediaFormat format=MediaFormat::MP3;char nas_username[64]={},nas_password[64]={};};
struct Suspend{bool active=true;AudioMusicSuspendOwner owner=AudioMusicSuspendOwner::Nsf;uint32_t track_index=4,playback_revision=9;AudioPlaybackSource source=AudioPlaybackSource::NasHttp;MediaFormat format=MediaFormat::MP3;bool has_technical_info=false;MediaTechnicalInfo technical_info;uint64_t position_ms=0;const char*path="http://nas/track/4";const char*nas_username="user";const char*nas_password="secret";};
static Suspend g_music_deep_suspend;
static uint32_t g_task_last_request_id=0,g_task_track_index=4,g_task_playback_revision=9,g_latest_transport_intent_revision=7;
static bool g_video_mp3_active=false,g_nsf_active=false;
static AudioPlaybackState g_task_state=AudioPlaybackState::Paused;
static int open_calls=0,resume_calls=0,shutdown_calls=0,releases=0,sync_calls=0;
static bool complete_success=false,fail_network=false,new_intent_during_open=false,queue_ok=true;
static std::vector<AudioRequest> queued;
struct AudioStateSnapshot{bool ready=true;AudioPlaybackSource source=AudioPlaybackSource::NasHttp;uint32_t track_index=4,playback_revision=9;};
bool audio_service_get_snapshot(AudioStateSnapshot*s){*s={};return true;}
const char*audio_music_suspend_owner_name(AudioMusicSuspendOwner){return "owner";}
const char*audio_playback_state_name_cn(AudioPlaybackState){return "state";}
const char*audio_music_suspend_ram_label(AudioMusicSuspendOwner,bool,bool){return "RAM";}
const char*esp_err_to_name(int){return "error";}
const char*pcm_seek_method_name(PcmSeekMethod){return "seek";}
void audio_task_log_ram(const char*){}
PcmDecoderType audio_decoder_type_for_format(MediaFormat f){return f==MediaFormat::MP3?PcmDecoderType::Mp3:PcmDecoderType::Flac;}
void audio_request_complete(AudioRequest*,bool success,int){complete_success=success;}
void audio_task_set_state(AudioPlaybackState s,int,AudioFailureScope=AudioFailureScope::System){g_task_state=s;}
void audio_task_release_music_deep_suspend_context(){g_music_deep_suspend.active=false;g_music_deep_suspend.path=nullptr;releases++;}
int audio_task_shutdown_pipeline(){shutdown_calls++;return 0;}
struct Clock{};static Clock g_playback_clock;uint64_t audio_playback_clock_position_ms(Clock*){return 0;}
void audio_task_handle_resume(AudioRequest*r){assert(r->track_index==g_task_track_index&&r->expected_playback_revision==g_task_playback_revision);resume_calls++;g_task_state=AudioPlaybackState::Playing;audio_request_complete(r,true,0);}
bool audio_command_is_transport_intent(AudioCommandType t){return t==AudioCommandType::Play||t==AudioCommandType::Seek;}
'''
 body=function(audio,'static uint32_t audio_transport_latest_revision()')+function(audio,'static bool audio_transport_request_is_latest(')+function(audio,'static bool audio_task_nas_open_should_abort(')
 body+='\nstatic const AudioRequest* nas_policy(const AudioRequest*request,bool check_transport_intent){\n'+policy+'\nreturn nas_abort_request;}\n'
 body+=r'''
int audio_task_start_pcm_pipeline(PcmDecoderType,const char*path,const AudioRequest*r,bool seek,uint64_t position,PcmSeekResult*,void*,bool check){
 open_calls++;assert(path&&*path);
 if(r->source==AudioPlaybackSource::NasHttp){assert(!seek&&position==0);assert(std::string(r->nas_username)=="user"&&std::string(r->nas_password)=="secret");}
 else assert(seek&&position==12345);
 if(audio_task_nas_open_should_abort(nas_policy(r,check)))return ESP_ERR_INVALID_STATE;
 if(fail_network)return ESP_FAIL;
 if(new_intent_during_open)g_latest_transport_intent_revision++;
 return ESP_OK;
}
AudioRequest*audio_request_create(AudioCommandType type,bool wait){assert(!wait);auto*r=new AudioRequest;r->type=type;return r;}
void audio_request_release(AudioRequest*r){delete r;}
bool audio_service_submit(AudioRequest*r,bool wait){assert(!wait);if(queue_ok)queued.push_back(*r);delete r;return queue_ok;}
'''
 body+=function(audio,'static void audio_task_handle_music_deep_restore(')
 body+=function(audio,'static bool audio_service_bind_current_playback_context(')
 body+=function(audio,'bool audio_service_music_deep_restore_async(')
 body+=r'''
static bool g_nsf_audio_active=true,g_music_deep_suspended_for_nsf=true,g_music_paused_for_nsf=true;
static bool g_nsf_paused=false,g_nsf_eof=false,g_nsf_failed=false,g_nsf_final_high_fps=false;
static unsigned g_last_nsf_eof_revision=0,g_nsf_visual_window_count=0,g_last_nsf_visual_snapshot_tick=0;
static void*g_timer=nullptr;static int kBrowserTimerPeriodMs=20;
bool audio_service_nsf_stop(bool restore,bool wait){assert(wait&&!restore);g_nsf_active=false;return true;}
void remote_context_end(){}void lv_timer_set_period(void*,int){}
bool audio_service_music_deep_restore(AudioMusicSuspendOwner,bool){sync_calls++;return false;}
bool audio_service_resume(bool){sync_calls++;return false;}
static bool g_music_paused_for_benchmark=true,g_music_deep_suspended_for_benchmark=true;
bool benchmark_music_exclusive_active(){return g_music_paused_for_benchmark||g_music_deep_suspended_for_benchmark;}
void cancel_browser_load(){}void waterfall_release_double_buffer(const char*){}void cancel_nsf_player(){}
void lv_timer_pause(void*){}void gesture_router_reset(){}void app_launcher_overlay_hide(){}void set_visible(void*,bool){}
static void*g_root=nullptr;enum class VisualMusicPage{Browser};static VisualMusicPage g_page=VisualMusicPage::Browser;
enum class AppRunState{Hidden};
'''
 body+=function(nsf,'static bool stop_nsf_audio(')+function(nsf,'static esp_err_t visual_music_leave(')
 body+=function(video,'static void benchmark_restore_music_after_exclusive(')
 body+=r'''
enum class VideoPage{Browser,Benchmark};static VideoPage g_video_page=VideoPage::Browser;
static bool g_profile_probe_pending=false,g_risk_prompt_active=false;
namespace VideoProbe{void cancel(){}}
struct BenchmarkUi{bool cleanup_pending=false;};static BenchmarkUi g_benchmark_ui;
static int kDestroyCleanupWaitMs=100;enum class EofTransition{None};static EofTransition g_eof_transition=EofTransition::None;
bool benchmark_request_stop(const char*){return true;}bool benchmark_presenter_is_running(){return false;}
int benchmark_wait_cleanup(int){return 0;}void show_browser(){g_video_page=VideoPage::Browser;}
'''
 body+=function(video,'static esp_err_t video_leave(').replace('g_page','g_video_page')

 test=r'''
void reset(AudioMusicSuspendOwner owner){g_music_deep_suspend={};g_music_deep_suspend.owner=owner;g_task_state=AudioPlaybackState::Paused;g_latest_transport_intent_revision=7;open_calls=resume_calls=shutdown_calls=releases=sync_calls=0;complete_success=false;fail_network=new_intent_during_open=false;queue_ok=true;queued.clear();g_nsf_active=g_video_mp3_active=false;}
int main(){
 // Root regression: synthetic revision zero must not be tested as a normal Play.
 reset(AudioMusicSuspendOwner::Nsf);AudioRequest synthetic;synthetic.transport_intent_revision=0;
 assert(audio_task_nas_open_should_abort(&synthetic));assert(!audio_task_nas_open_should_abort(nas_policy(&synthetic,false)));
 assert(audio_task_nas_open_should_abort(nas_policy(&synthetic,true)));
 for(auto owner:{AudioMusicSuspendOwner::Nsf,AudioMusicSuspendOwner::Video}){
  // Existing synchronous API stays compatible and no longer self-cancels NAS.
  reset(owner);AudioRequest legacy;legacy.music_suspend_owner=owner;audio_task_handle_music_deep_restore(&legacy);
  assert(complete_success&&open_calls==1&&resume_calls==0&&g_task_state==AudioPlaybackState::Paused);
  for(bool resume:{false,true}){reset(owner);assert(audio_service_music_deep_restore_async(owner,resume));assert(open_calls==0&&queued.size()==1);
   auto r=queued.back();audio_task_handle_music_deep_restore(&r);
   assert(complete_success&&open_calls==1&&resume_calls==int(resume)&&!g_music_deep_suspend.active);
   assert(g_task_state==(resume?AudioPlaybackState::Playing:AudioPlaybackState::Paused));
  }
  reset(owner);assert(audio_service_music_deep_restore_async(owner,true));auto r=queued.back();fail_network=true;audio_task_handle_music_deep_restore(&r);
  assert(!complete_success&&resume_calls==0&&!g_music_deep_suspend.active&&g_task_state==AudioPlaybackState::Error&&shutdown_calls==1);
  reset(owner);assert(audio_service_music_deep_restore_async(owner,true));r=queued.back();g_latest_transport_intent_revision++;audio_task_handle_music_deep_restore(&r);
  assert(!complete_success&&open_calls==0&&resume_calls==0&&!g_music_deep_suspend.active);
  reset(owner);assert(audio_service_music_deep_restore_async(owner,true));r=queued.back();new_intent_during_open=true;audio_task_handle_music_deep_restore(&r);
  assert(!complete_success&&resume_calls==0&&!g_music_deep_suspend.active&&shutdown_calls==1);
 }
 // Local restoration still seeks to its original position through the sync path.
 reset(AudioMusicSuspendOwner::Nsf);g_music_deep_suspend.source=AudioPlaybackSource::Local;g_music_deep_suspend.position_ms=12345;
 AudioRequest local;audio_task_handle_music_deep_restore(&local);assert(complete_success&&open_calls==1&&resume_calls==0&&g_task_state==AudioPlaybackState::Paused);
 // Wrong owner must not steal another exclusive session's saved track.
 reset(AudioMusicSuspendOwner::Nsf);AudioRequest wrong;wrong.music_suspend_owner=AudioMusicSuspendOwner::Video;
 audio_task_handle_music_deep_restore(&wrong);assert(!complete_success&&open_calls==0&&g_music_deep_suspend.active);
 // Real NSF leave can finish before any HTTP open, including a later network failure.
 reset(AudioMusicSuspendOwner::Nsf);g_nsf_audio_active=g_music_deep_suspended_for_nsf=g_music_paused_for_nsf=true;
 assert(visual_music_leave(AppRunState::Hidden)==ESP_OK);assert(queued.size()==1&&open_calls==0&&sync_calls==0&&!g_music_deep_suspended_for_nsf&&!g_music_paused_for_nsf);
 fail_network=true;auto r=queued.back();audio_task_handle_music_deep_restore(&r);assert(g_task_state==AudioPlaybackState::Error&&!g_music_deep_suspend.active);
 // Video release uses the same queued mechanism, preserving pre-entry paused intent.
 reset(AudioMusicSuspendOwner::Video);g_music_deep_suspended_for_benchmark=true;g_music_paused_for_benchmark=false;
 benchmark_restore_music_after_exclusive("leave");assert(queued.size()==1&&!queued.back().music_restore_resume&&open_calls==0&&sync_calls==0);
 reset(AudioMusicSuspendOwner::Video);g_music_deep_suspended_for_benchmark=g_music_paused_for_benchmark=true;
 assert(video_leave(AppRunState::Hidden)==ESP_OK&&queued.size()==1&&open_calls==0&&sync_calls==0);
 fail_network=true;r=queued.back();audio_task_handle_music_deep_restore(&r);assert(g_task_state==AudioPlaybackState::Error);
 reset(AudioMusicSuspendOwner::Video);g_music_deep_suspended_for_benchmark=g_music_paused_for_benchmark=true;queue_ok=false;
 assert(video_leave(AppRunState::Hidden)==ESP_ERR_TIMEOUT&&g_music_deep_suspended_for_benchmark&&queued.empty());
 // Queue admission failure keeps lifecycle ownership for an explicit retry.
 reset(AudioMusicSuspendOwner::Nsf);queue_ok=false;g_nsf_audio_active=g_music_deep_suspended_for_nsf=g_music_paused_for_nsf=true;
 assert(visual_music_leave(AppRunState::Hidden)!=ESP_OK);assert(g_music_deep_suspended_for_nsf&&g_music_paused_for_nsf&&queued.empty());
 puts("PASS: actual NAS abort policy, NSF/Video synchronous+async restore, paused/playing intent, credentials, newer target cancellation, network failure cleanup and NSF/Video leave without HTTP wait");
}
'''
 with tempfile.TemporaryDirectory() as d:
  p=Path(d)/'test.cpp';p.write_text(header+body+test)
  subprocess.run(['g++','-std=c++17','-Wall',str(p),'-o',str(Path(d)/'test')],check=True)
  subprocess.run([str(Path(d)/'test')],check=True)
if __name__=='__main__':main()
