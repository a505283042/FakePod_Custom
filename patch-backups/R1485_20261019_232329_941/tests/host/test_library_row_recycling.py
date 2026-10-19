#!/usr/bin/env python3
"""Run actual virtual-row refresh with mock LVGL and row binders."""
from pathlib import Path
import subprocess,tempfile,sys,random
root=Path(__file__).resolve().parents[2]
s=Path(sys.argv[1]).read_text() if len(sys.argv)>1 else (root/'src/ui/screens/library_view.cpp').read_text()
a=s.index('static void library_view_refresh_virtual_rows(bool force)\n');b=s.index('\nstatic void library_view_show_empty_if_needed',a)
fn=s[a:b]
code=r'''
#include <cassert>
#include <cstdint>
#include <set>
#include <random>
constexpr uint32_t LIBRARY_VIRTUAL_ROWS=7;
struct Obj{int y=0;bool hidden=true;};
struct Binding{uint32_t position=UINT32_MAX;};
struct LibraryVirtualRow{Obj*button=nullptr;Binding binding{};uint32_t item_index=UINT32_MAX;};
Obj objects[7],host;Obj*g_list_host=&host;LibraryVirtualRow g_rows[7];
enum class LibraryBrowseMode{AllTracks,GroupTracks};struct State{LibraryBrowseMode mode=LibraryBrowseMode::AllTracks;}g_state;
uint32_t count=376;int scroll=0,step=70,binds=0;bool folder=false,search=false;
uint32_t library_view_item_count(){return count;}int library_view_row_step(){return step;}int library_view_current_scroll_y(){return scroll;}
uint32_t library_view_source_item_index(uint32_t i){return search?10000+i*3:i;}
void library_view_hide_row(LibraryVirtualRow&r){r.item_index=UINT32_MAX;r.binding={};r.button->hidden=true;}
void lv_obj_set_y(Obj*p,int y){p->y=y;}constexpr int LV_OBJ_FLAG_HIDDEN=1;
void lv_obj_remove_flag(Obj*p,int){p->hidden=false;}
bool library_view_folder_browser_active(){return folder;}
bool bind(LibraryVirtualRow&r,uint32_t i){++binds;r.binding.position=i;return true;}
bool library_view_bind_folder_row(LibraryVirtualRow&r,uint32_t i){return bind(r,i);}
bool library_view_bind_detail_row(LibraryVirtualRow&r,uint32_t i){return bind(r,i);}
bool library_view_bind_top_row(LibraryVirtualRow&r,uint32_t i){return bind(r,i);}
'''+fn+r'''
void verify(){
 uint32_t first_visible=scroll>0?scroll/step:0,first=first_visible?first_visible-1:0;
 std::set<uint32_t>seen;
 for(auto&r:g_rows){if(r.button->hidden){assert(r.item_index==UINT32_MAX);continue;}
  assert(r.item_index>=first&&r.item_index<first+7&&r.item_index<count);
  assert(r.binding.position==library_view_source_item_index(r.item_index));
  assert(r.button->y==int(r.item_index)*step+4-scroll);
  assert(seen.insert(r.item_index).second);
 }
 for(uint32_t i=first;i<first+7&&i<count;++i)assert(seen.count(i));
}
int main(){
 for(unsigned i=0;i<7;++i)g_rows[i].button=&objects[i];
 library_view_refresh_virtual_rows(true);assert(binds==7);verify();
 scroll=step;binds=0;library_view_refresh_virtual_rows(false);assert(binds==0);verify();
 scroll=step*2;binds=0;library_view_refresh_virtual_rows(false);assert(binds==1);verify();
 scroll=step;binds=0;library_view_refresh_virtual_rows(false);assert(binds==1);verify();
 std::mt19937 rng(1483);
 for(unsigned mode=0;mode<3;++mode){folder=mode==2;g_state.mode=mode==1?LibraryBrowseMode::GroupTracks:LibraryBrowseMode::AllTracks;
  for(unsigned total:{0U,1U,2U,6U,7U,8U,376U,4227U}){
   count=total;search=!search;scroll=0;library_view_refresh_virtual_rows(true);verify();
   for(unsigned j=0;j<1000;++j){scroll=total?int(rng()%(total*step)):0;library_view_refresh_virtual_rows(false);verify();}
   // Context/search changes must force rebind, even at same scroll offset.
   search=!search;library_view_refresh_virtual_rows(true);verify();
  }
 }
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-O2','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 import os
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
print('PASS actual row refresh: one new bind vs seven, reverse/jumps/empty/tail/search/detail/folder; ASan/UBSan')
