"""Host numerical checks, not an ESP32-S3 timing benchmark.
Optional --dsp-reference points to ESP-DSP v1.6.0 dsps_fft2r_fc32_ansi.c.
"""
import argparse,os,subprocess,tempfile
from pathlib import Path
r=Path(__file__).resolve().parents[2]
a=argparse.ArgumentParser();a.add_argument('--dsp-reference',type=Path);args=a.parse_args()
reference=''
if args.dsp_reference:
 s=args.dsp_reference.read_text();start=s.index('esp_err_t dsps_fft2r_fc32_ansi_(');brace=s.index('{',start);depth=1;end=brace+1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 reference='''using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_DSP_INVALID_LENGTH=1,ESP_ERR_DSP_UNINITIALIZED=2;
static int dsps_fft2r_initialized=1;
static bool dsp_is_power_of_two(int n){return n>0 && (n&(n-1))==0;}
'''+s[start:end]
 reference+='''\nstatic void reference_fft(float *data){
 if(dsps_fft2r_fc32_ansi_(data,256,const_cast<float*>(fakepod_spectrum::kTwiddle)))std::abort();
 fakepod_spectrum::bit_reverse(data);}
'''
else:reference='static void reference_fft(float *data){fakepod_spectrum::portable_fft(data);fakepod_spectrum::bit_reverse(data);}'
code=r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <vector>
#include "src/audio/spectrum_fft_math.h"
#include "src/audio/decoders/pcm_i2s_convert.h"
REFERENCE
static uint8_t old_level(float power){
 float n=std::max(sqrtf(power)/64.0f,0.000001f);float db=20.0f*log10f(n);
 if(db<=-45)return 0;
 if(db>=0)return 255;
 uint32_t x=uint32_t((db+45)*(255.0f/45)+0.5f);return uint8_t((x*x+127)/255);
}
static int32_t old_sample(const uint8_t *p,unsigned bits){
 uint32_t raw=uint32_t(p[0])|(uint32_t(p[1])<<8);
 if(bits==16){uint32_t extended=(raw&0x8000)?raw|0xffff0000:raw;return int32_t(extended<<16);}
 raw|=uint32_t(p[2])<<16;
 if(bits==24){uint32_t extended=(raw&0x800000)?raw|0xff000000:raw;return int32_t(extended<<8);}
 return int32_t(raw|(uint32_t(p[3])<<24));
}
int main(){
 std::mt19937 rng(1476);int16_t samples[256];alignas(16) float data[512],other[512];
 assert(reinterpret_cast<uintptr_t>(fakepod_spectrum::kTwiddle)%16==0);
 for(int trial=0;trial<40;++trial){
  for(int i=0;i<256;++i){samples[i]=trial==0?0:trial==1?12345:trial<12?int16_t(30000*sin(2*3.141592653589793*(trial-1)*i/256)):int16_t(rng());}
  fakepod_spectrum::prepare(samples,data);std::copy(data,data+512,other);
  float mean=0;for(int i=0;i<256;++i)mean+=samples[i];mean/=256;
  for(int i=0;i<256;++i){float w=i<=128?float(i)/128:float(255-i)/127;
   assert(fabs(data[2*i]-((float(samples[i])-mean)/32768)*w)<2e-7);assert(data[2*i+1]==0);}
  fakepod_spectrum::portable_fft(data);fakepod_spectrum::bit_reverse(data);reference_fft(other);
  for(int k=0;k<256;++k){
   double re=0,im=0;for(int i=0;i<256;++i){float w=i<=128?float(i)/128:float(255-i)/127;
    double v=((float(samples[i])-mean)/32768)*w,angle=-2*3.141592653589793*k*i/256;
    re+=v*cos(angle);im+=v*sin(angle);}
   assert(fabs(data[2*k]-re)<0.0002 && fabs(data[2*k+1]-im)<0.0002);
   assert(fabs(other[2*k]-re)<0.0002 && fabs(other[2*k+1]-im)<0.0002);
  }
 }
 for(int i=0;i<200000;++i){float p=powf(10.0f,-15.0f+float(i)/10000);
  assert(abs(int(old_level(p))-int(fakepod_spectrum::power_level(p)))<=1);}
 assert(fakepod_spectrum::power_level(0)==0);
 for(unsigned bits:{16U,24U,32U})for(unsigned channels:{1U,2U})for(int offset=0;offset<4;++offset){
  size_t frames=65536,bytes=bits/8;std::vector<uint8_t> input(offset+frames*channels*bytes+8);
  for(auto &b:input)b=uint8_t(rng());
  if(bits==16)for(size_t i=0;i<frames*channels;++i){input[offset+i*2]=uint8_t(i);input[offset+i*2+1]=uint8_t(i>>8);}
  // Ensure 24/32-bit extrema in addition to random values and unaligned addresses.
  for(size_t i=0;i<bytes;++i)input[offset+i]=0;
  input[offset+bytes-1]=0x80;
  for(size_t i=0;i<bytes;++i)input[offset+bytes+i]=0xff;
  std::vector<int32_t> output(frames*2+2,int32_t(0x13572468));
  fakepod_pcm::to_i2s32(input.data()+offset,output.data()+1,frames,bits,channels);
  assert(output.front()==int32_t(0x13572468) && output.back()==int32_t(0x13572468));
  for(size_t i=0;i<frames;++i){auto p=input.data()+offset+i*channels*bytes;
   assert(output[1+2*i]==old_sample(p,bits));assert(output[2+2*i]==old_sample(p+(channels==2?bytes:0),bits));}
  fakepod_pcm::to_i2s32(input.data(),output.data(),0,bits,channels);
 }
 puts("PASS: window/DC removal; FFT vs direct DFT; power level tolerance <=1; PCM bit-exact 16/24/32-bit mono/stereo with unaligned input and guards");
}
'''.replace('REFERENCE',reference)
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-fsanitize=undefined,address','-I',str(r),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
print('Official ESP-DSP ANSI reference verified' if args.dsp_reference else 'Portable implementation verified; pass --dsp-reference for official ANSI comparison')
