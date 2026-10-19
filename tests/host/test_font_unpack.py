#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
code=r'''
#include <cassert>
#include <vector>
#include <random>
#include "font_unpack_2bpp.h"
int main(){std::mt19937 rng(1484);
 for(unsigned w=1;w<=96;++w)for(unsigned h:{1U,17U,32U,65U})for(unsigned pad=0;pad<8;++pad){
  unsigned stride=w+pad;std::vector<uint8_t>s(((w+3)/4)*h),dst(stride*h+16,0xAB),ref=dst;
  for(auto&v:s)v=rng();
  for(unsigned y=0;y<h;++y){for(unsigned x=0;x<w;++x)ref[y*stride+x]=((s[y*((w+3)/4)+x/4]>>(6-2*(x%4)))&3)*85;
   for(unsigned x=w;x<stride;++x)ref[y*stride+x]=0;
  }
  fakepod_font::unpack(s.data(),dst.data(),w,h,stride);assert(dst==ref);
 }
 for(unsigned b=0;b<256;++b)for(unsigned x=0;x<4;++x)assert(fakepod_font::alpha_lut[b][x]==((b>>(6-2*x))&3)*85);
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-O2','-fsanitize=address,undefined','-I',str(root/'src/ui/font'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
print('PASS: exact glyph alpha/stride/padding/guards, every packed byte; ASan/UBSan')
