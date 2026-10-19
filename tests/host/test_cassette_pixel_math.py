#!/usr/bin/env python3
"""Check exact pixel output against the original implementation."""
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[2]
s=(root/'src/ui/screens/cassette_view.cpp').read_text()
def extract(name):
 a=s.index('static void '+name+'(');b=s.index('{',a);i=b+1;depth=1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
cover=extract('cassette_view_blit_cover');dim=extract('cassette_view_dim_buffer')
code=r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include <random>
#include "cassette_pixel_math.h"
constexpr int kCassetteX=0,kCassetteY=68,kLabelX=30,kLabelY=25,kLabelWidth=400,kLabelHeight=186;
constexpr unsigned kLvImageScaleNone=256,kControlsCacheWidth=460,kControlsCacheHeight=460;
constexpr uint8_t kControlsCacheDimOpacity=150;
void vTaskDelay(int){}
uint16_t cassette_view_load_rgb565(const uint8_t*p){return p[0]|uint16_t(p[1])<<8;}
uint32_t test_scale=256;
uint32_t cassette_view_cover_scale_for_size(uint16_t,uint16_t){return test_scale;}
'''+cover+dim+r'''
int main(){
 for(uint32_t scale=1;scale<=4096;++scale){
  for(uint32_t start: {0U,1U,93U,1024U}){
   fakepod_cassette::ScaleCursor c(start,scale);
   for(uint32_t i=0;i<460;++i,c.advance())assert(c.value==(uint64_t(start+i)*256)/scale);
  }
 }
 std::vector<uint16_t> colors(65536);
 for(unsigned i=0;i<65536;++i){
  const unsigned keep=105,r=(i>>11)&31,g=(i>>5)&63,b=i&31;
  const uint16_t expected=(((r*keep+127)/255)<<11)|(((g*keep+127)/255)<<5)|((b*keep+127)/255);
  assert(fakepod_cassette::Dim565<150>::apply(i)==expected);
 }
 std::mt19937 rng(1479);
 for(int trial=0;trial<100;++trial){
  uint16_t w=1+rng()%1000,h=1+rng()%1000;test_scale=1+rng()%1500;
  std::vector<uint8_t> src(size_t(w)*h*2);for(auto &v:src)v=rng();
  std::vector<uint16_t> actual(460*460,0x1234),expected=actual;
  int16_t offset=int(rng()%501)-250;
  int sw=uint64_t(w)*test_scale/256,sh=uint64_t(h)*test_scale/256;
  int left=kCassetteX+kLabelX+(kLabelWidth-sw)/2,top=kCassetteY+kLabelY+(kLabelHeight-sh)/2+offset;
  for(int y=93;y<279;++y)for(int x=30;x<430;++x){
   int sx=x-left,sy=y-top;if(sx<0||sx>=sw||sy<0||sy>=sh)continue;
   unsigned ix=uint64_t(sx)*256/test_scale,iy=uint64_t(sy)*256/test_scale;
   if(ix>=w)ix=w-1;if(iy>=h)iy=h-1;
   expected[y*460+x]=cassette_view_load_rgb565(src.data()+(size_t(iy)*w+ix)*2);
  }
  cassette_view_blit_cover(actual.data(),src.data(),w,h,offset);assert(actual==expected);
  cassette_view_dim_buffer(actual.data());
  for(size_t i=0;i<actual.size();++i)assert(actual[i]==fakepod_cassette::Dim565<150>::apply(expected[i]));
 }
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-O2','-fsanitize=address,undefined','-I',str(root/'src/ui/screens'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 import os
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
print('PASS: all RGB565 values, 4096 scales, clipped actual blit/dim routines; ASan/UBSan')
