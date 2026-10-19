#!/usr/bin/env python3
"""Exercise startup, refill and exact high-water/time boundaries."""
from pathlib import Path
import tempfile, subprocess
root=Path(__file__).resolve().parents[2]
code=r'''#include <cassert>
#include <initializer_list>
#include "nas_stream_schedule.h"
int main(){for(size_t c:{131072U,393216U})for(size_t b=0;b<=c;++b){
assert(!fakepod_nas::should_cooperate(false,b,c,999999));
assert(!fakepod_nas::should_cooperate(true,b,c,1999));
assert(fakepod_nas::should_cooperate(true,b,c,2000)==(b>=c-c/4));
}assert(!fakepod_nas::should_cooperate(true,0,0,3000));}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(root/'src/audio/sources'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: NAS scheduling startup, watermarks and time boundaries')
