#!/usr/bin/env python3
"""Run actual refill/read functions with a parser that caches its final frame."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[2];src=(root/'src/audio/decoders/flac_decoder.cpp').read_text()
def extract(name):
 a=src.index(name);a=src.rfind('\n',0,a)+1;b=src.index('{',a);i=b+1;depth=1
 while depth:
  depth+=(src[i]=='{')-(src[i]=='}');i+=1
 return src[a:i]
refill=extract('static esp_err_t flac_decode_next_output(')
read=extract('esp_err_t flac_decoder_read_pcm32(')
# Exercise the real early-EOS branch, not a separately reimplemented policy.
a=src.index('    if (decoder->input_chunk_eos) {', src.index('static esp_err_t flac_prepare_input_window('))
b=src.index('    const size_t target =',a)
early=src[a:b].replace('decoder->','d->')

code=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "flac_eos_policy.h"
#include "pcm_i2s_convert.h"
#define APP_DIAG_FLAC_PERFORMANCE 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
using esp_err_t=int;using esp_audio_err_t=int;using esp_audio_simple_dec_handle_t=void*;
constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_SIZE=2,ESP_ERR_INVALID_RESPONSE=3,ESP_ERR_INVALID_STATE=4,ESP_ERR_TIMEOUT=5;
constexpr int ESP_AUDIO_ERR_OK=0,ESP_AUDIO_ERR_BUFF_NOT_ENOUGH=6;
constexpr int FLAC_MAX_DECODED_BUFFER_BYTES=1000000;
struct FlacDecoder{void*simple_handle=(void*)1;uint8_t*input_buffer;uint8_t*decoded_buffer;size_t input_offset=0,input_size=0,decoded_offset=0,decoded_size=0,decoded_capacity=24576;bool eof=false,input_chunk_eos=false;uint64_t total_frames=24552432,frames_read=24551424;uint32_t max_block_size=4096;uint8_t bits_per_sample=24,channels=2;};
struct esp_audio_simple_dec_raw_t{uint8_t*buffer;uint32_t len,consumed=0;bool eos=false;};
struct esp_audio_simple_dec_out_t{uint8_t*buffer;uint32_t len,decoded_size=0,needed_size=0;};
int calls=0,flushes=0,mode=0;bool first=true;
esp_err_t flac_prepare_input_window(FlacDecoder*d,bool*){
 size_t remaining=d->input_offset<d->input_size?d->input_size-d->input_offset:0;
'''+early+r'''
 if(first){first=false;d->input_size=100;d->input_offset=0;d->input_chunk_eos=true;}
 else {d->input_size=d->input_offset=0;d->input_chunk_eos=true;
 if(!fakepod_flac::pending_tail(d->total_frames,d->frames_read,d->max_block_size))d->eof=true;}
 return 0;
}
int esp_audio_simple_dec_process(void*,esp_audio_simple_dec_raw_t*r,esp_audio_simple_dec_out_t*o){
 ++calls;if(r->len){if(mode!=4)r->consumed=r->len;return 0;}
 ++flushes;assert(r->eos);
 if(mode==3)return 7;
 if(mode==1)return 0;
 if(o->len<6048){o->needed_size=6048;return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;}
 o->decoded_size=mode==2?6000:6048;
 for(unsigned i=0;i<o->decoded_size;++i)o->buffer[i]=uint8_t(i);
 return 0;
}
int flac_audio_error_to_esp(int){return 7;}
int flac_resize_decoded_buffer(FlacDecoder*d,size_t n){d->decoded_capacity=n;return 0;}
int flac_verify_runtime_info(FlacDecoder*){return 0;}
bool flac_decoder_is_open(FlacDecoder*d){return d&&d->simple_handle;}
'''+refill+read+r'''
int main(){
 uint8_t input[32768]{},pcm[24576]{};int32_t out[512]{};
 auto reset=[&](){calls=flushes=0;first=true;FlacDecoder d;d.input_buffer=input;d.decoded_buffer=pcm;return d;};
 auto d=reset();size_t sum=0,n=0;
 do{assert(flac_decoder_read_pcm32(&d,out,256,&n)==0);sum+=n;}while(n);
 assert(sum==1008&&d.frames_read==d.total_frames&&calls==2&&flushes==1);
 assert(d.eof);assert(flac_decoder_read_pcm32(&d,out,256,&n)==0&&n==0&&flushes==1);
 mode=0;d=reset();d.decoded_capacity=8;assert(flac_decoder_read_pcm32(&d,out,256,&n)==0&&n==256&&flushes==2);
 for(mode=1;mode<=4;++mode){d=reset();assert(flac_decoder_read_pcm32(&d,out,256,&n)!=0);assert(n==0);}
 assert(!fakepod_flac::pending_tail(0,0,4096));
 assert(!fakepod_flac::pending_tail(100,100,4096));
 assert(!fakepod_flac::pending_tail(5000,0,4096));
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-O2','-fsanitize=address,undefined','-I',str(root/'src/audio/decoders'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
print('PASS actual refill/read: 1008-frame cached tail drains once; small output retry, missing/mismatched/error tails rejected')
