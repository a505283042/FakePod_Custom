from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
code=r'''
#include "library_row_step.h"
#include <cassert>
#include <random>
int main(){using namespace fakepod_library;
 for(int step:{50,70}) {
  int r=0,total=0,output=0;std::mt19937 rng(1484);
  for(int i=0;i<100000;++i){int d=int(rng()%401)-200;total+=d;int q=consume_drag(r,d,step);output+=q;assert(q%step==0);assert(r>-step&&r<step);assert(output+r==total);}
  r=0;for(int i=0;i<step-1;++i)assert(consume_drag(r,1,step)==0);assert(consume_drag(r,1,step)==step);
  assert(consume_drag(r,-step*3-4,step)==-step*3);assert(r==-4);
  for(int count:{0,1,5,6,7,376,4227}) {int max=count>5?(count-5)*step:0;
   for(int y=-100;y<max+100;++y){int v=align_scroll(y,max,step);assert(v>=0&&v<=max&&v%step==0);}
   assert(align_scroll(max+999,max,step)==max);
  }
 }
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'t.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-O2','-fsanitize=undefined','-I'+str(root/'src/ui/screens'),str(p/'t.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
s=(root/'src/ui/screens/library_view.cpp').read_text();feed=s.split('void library_view_feed_pointer(',1)[1].split('bool library_view_search_is_active',1)[0]
assert 'library_view_inertia_start(' not in feed
assert 'consume_drag(' in feed and 'row_drag_remainder = 0' in feed
print('PASS: drag accumulation/reverse/multi-row, whole-row bounds, no release inertia; UBSan')
