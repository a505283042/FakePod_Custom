#!/usr/bin/env python3
"""Exercise actual frame-switch code, verify fixed geometry and strip mapping."""
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[2];s=(root/'src/ui/screens/cassette_view.cpp').read_text()
a=s.index('static void cassette_view_apply_mechanics_frames(');b=s.index('\nstatic lv_obj_t *cassette_view_create_tape_line',a)
fn=s[a:b]
code=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
struct Obj{int x=0,offset=0,w=56;};
constexpr uint8_t kBigReelFrameCount=12,kSmallRollerFrameCount=2;
constexpr int kBigReelSize=56,kSmallRollerSize=44;
bool g_mechanics_ready=true;Obj big[2],small[2];
Obj*g_big_reel_strip_images[2]={&big[0],&big[1]};Obj*g_small_roller_strip_images[2]={&small[0],&small[1]};
uint8_t g_last_big_reel_frame[2]={255,255},g_last_small_roller_frame=255;unsigned writes=0;
void lv_image_set_offset_x(Obj*p,int v){p->offset=v;++writes;}
'''+fn+r'''
int main(){
 for(unsigned f=0;f<48;++f){
  unsigned l=f%12,r=(f*3)%12,t=f%2;
  cassette_view_apply_mechanics_frames(l,r,t);
  assert(big[0].x==0&&big[1].x==0&&small[0].x==0&&small[1].x==0);
  assert(big[0].offset==-int(l*56)&&big[1].offset==-int(r*56));
  assert(small[0].offset==-int(t*44)&&small[1].offset==-int(t*44));
  unsigned n=writes;cassette_view_apply_mechanics_frames(l,r,t);assert(writes==n);
  for(int x=0;x<56;++x){
   int old_source=x+int(l*56);
   int tiled_source=(x-big[0].offset)%(12*56);
   assert(old_source==tiled_source);
  }
 }
 unsigned n=writes;g_mechanics_ready=false;cassette_view_apply_mechanics_frames(1,1,1);assert(writes==n);
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-O2',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
assert 'lv_image_set_inner_align(strip, LV_IMAGE_ALIGN_TILE)' in s
assert 'lv_obj_set_size(strip, size, size)' in s
print('PASS: actual sprite frames, fixed object coordinates, same-frame skip, strip pixel mapping')
