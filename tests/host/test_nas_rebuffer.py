#!/usr/bin/env python3
"""Compile actual recovery functions and actual pre-decode service gate with mocks."""
from pathlib import Path
import tempfile, subprocess
root=Path(__file__).resolve().parents[2]
s=(root/'src/audio/audio_service.cpp').read_text()
def function(name):
 a=s.index('static ',s.index(name)-20);b=s.index('{',a);depth=1;i=b+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
report=s[s.index('// AudioTask-owned low-frequency'):s.index('static void audio_task_reset_flac_starve_grace()')]
functions='\n'.join(function(n) for n in ['audio_task_reset_flac_starve_grace()', 'audio_task_log_flac_starve_recovered()', 'audio_task_try_recover_flac_starvation('])
a=s.index('static void audio_task_service_pcm_playback()');b=s.index('    size_t frames = 0;',a)
gate=s[a:b]+'    ++decode_calls;\n}\n'
code=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include "nas_rebuffer_policy.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_TIMEOUT=1;
enum class MediaFormat{FLAC,MP3};enum class AudioPlaybackSource{NasHttp,Local};
enum class AudioPlaybackState{Playing,Paused,Error};enum class AudioFailureScope{System};
struct BufferedHttpAudioSourceStats{uint32_t ring_buffered_bytes=0,ring_capacity_bytes=393216;bool eof=false,io_error=false;};
struct FlacPrefetchRuntimeSnapshot{bool active=true,io_error=false,eof=false;uint32_t buffered_bytes=0,capacity_bytes=262144;};
struct Decoder {int source=0,flac=0;}g_decoder;
auto g_task_format=MediaFormat::FLAC;auto g_task_source=AudioPlaybackSource::NasHttp;auto g_task_state=AudioPlaybackState::Playing;
uint32_t g_flac_starve_grace_attempts=0,g_task_sample_rate_hz=96000;int64_t g_flac_starve_grace_started_us=0,now=1000000;
constexpr uint32_t AUDIO_FLAC_STARVE_GRACE_MAX_MS=1000,AUDIO_FLAC_STARVE_GRACE_MAX_ATTEMPTS=32,AUDIO_HTTP_FLAC_STARVE_GRACE_MAX_MS=6000,AUDIO_HTTP_FLAC_STARVE_GRACE_MAX_ATTEMPTS=2048;
constexpr size_t AUDIO_STREAM_FRAMES=256;constexpr uint32_t AUDIO_I2S_WRITE_TIMEOUT_MS=100;
BufferedHttpAudioSourceStats stats;int silence_calls=0,decode_calls=0,fail_calls=0;bool silence_ok=true;
int64_t esp_timer_get_time(){return now;}
bool flac_decoder_is_open(int*){return true;}bool pcm_decoder_is_open(Decoder*){return true;}
bool buffered_http_audio_source_get_stats(int*,BufferedHttpAudioSourceStats*out){*out=stats;return true;}
bool flac_decoder_get_prefetch_runtime(int*,FlacPrefetchRuntimeSnapshot*out){*out={};return true;}
int i2s_output_stream_write_silence(size_t,uint32_t){++silence_calls;now+=11000;return silence_ok?ESP_OK:2;}
void audio_task_reset_music_last_pcm(){}
void audio_task_fail_stream(int,const char*,AudioFailureScope){++fail_calls;g_task_state=AudioPlaybackState::Error;}
'''+report+functions+gate+r'''
int main(){
 for(size_t b=0;b<=393216;++b){assert(fakepod_nas::wait_for_rebuffer(b,393216,false,false)==(b<262144));}
 assert(!fakepod_nas::wait_for_rebuffer(0,393216,true,false));
 assert(!fakepod_nas::wait_for_rebuffer(0,393216,false,true));
 assert(audio_task_try_recover_flac_starvation(ESP_ERR_TIMEOUT));
 assert(g_flac_starve_grace_attempts==1 && silence_calls==1);
 audio_task_service_pcm_playback();assert(decode_calls==0 && silence_calls==2);
 stats.ring_buffered_bytes=8192;audio_task_service_pcm_playback();assert(decode_calls==0);
 stats.ring_buffered_bytes=262144;audio_task_service_pcm_playback();assert(decode_calls==1);
 audio_task_log_flac_starve_recovered();assert(g_flac_starve_grace_attempts==0);
 stats.ring_buffered_bytes=0;assert(audio_task_try_recover_flac_starvation(ESP_ERR_TIMEOUT));
 stats.eof=true;audio_task_service_pcm_playback();assert(decode_calls==2);
 stats.eof=false;stats.io_error=true;audio_task_service_pcm_playback();assert(decode_calls==3);
 stats.io_error=false;g_task_state=AudioPlaybackState::Paused;int calls=silence_calls;
 audio_task_service_pcm_playback();assert(silence_calls==calls && decode_calls==3);
 g_task_state=AudioPlaybackState::Playing;now=g_flac_starve_grace_started_us+6000000;
 audio_task_service_pcm_playback();assert(fail_calls==1 && g_flac_starve_grace_attempts==0);
 g_task_state=AudioPlaybackState::Playing;now+=100;silence_ok=false;
 assert(!audio_task_try_recover_flac_starvation(ESP_ERR_TIMEOUT));assert(g_flac_starve_grace_attempts==0);
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-I',str(root/'src/audio/sources'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: actual recovery/gate: low/high water, EOF/error, paused command state, time cap, I2S failure')
