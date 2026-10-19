from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2];s=(root/'src/ui/screens/library_view.cpp').read_text()
a=s.index('static void library_view_marquee_stop()\n');b=s.index('static void library_view_row_push_finish',a)
code=r'''
#include <cassert>
#include <cstdint>
struct lv_anim_t{int32_t act_time=0,start_value=0,end_value=-100;};lv_anim_t animation;uint32_t g_marquee_move_ms=1;
constexpr int LV_ANIM_REPEAT_INFINITE=-1;
int library_abs(int x){return x<0?-x:x;}
void lv_anim_set_repeat_count(lv_anim_t*,int n){assert(n==-1);}
void lv_anim_set_playback_delay(lv_anim_t*,int n){assert(n==0);}
void lv_anim_set_repeat_delay(lv_anim_t*,int n){assert(n==1800);}
void lv_anim_set_playback_duration(lv_anim_t*,int n){assert(n==0);}
void lv_anim_set_duration(lv_anim_t*,uint32_t n){assert(n==g_marquee_move_ms+1800);}
void lv_anim_set_delay(lv_anim_t*,int n){assert(n==0);}
void lv_anim_set_path_cb(lv_anim_t*,int32_t(*)(const lv_anim_t*)){}
void lv_anim_start(lv_anim_t*){}
struct lv_obj_t{int mode=0;bool hidden=false;};struct lv_timer_t{};
enum{LV_LABEL_LONG_DOT,LV_LABEL_LONG_SCROLL,LV_OBJ_FLAG_HIDDEN,ScreenPowerNormal};
lv_obj_t label,button;struct Row{uint32_t item_index=UINT32_MAX;bool center_selected=false;lv_obj_t*button=nullptr;lv_obj_t*label=nullptr;};Row g_rows[7];
lv_obj_t*g_marquee_label=nullptr;uint32_t g_marquee_activity_tick=0,tick=0;bool visible=true,pressure=false,g_row_push_pending=false;
struct{bool pressed=false;}g_gesture;int power=ScreenPowerNormal,changes=0;
struct FlacStorageWindowSnapshot{bool active=true,pressure_active=false;};
bool flac_decoder_get_storage_window(FlacStorageWindowSnapshot*p){p->pressure_active=pressure;return true;}
bool library_view_is_visible(){return visible;}int screen_lock_simple_get_power(){return power;}
uint32_t lv_tick_get(){return tick;}void lv_label_set_long_mode(lv_obj_t*p,int m){p->mode=m;++changes;}
bool lv_obj_has_flag(lv_obj_t*p,int){return p->hidden;}const char*lv_label_get_text(lv_obj_t*){return "歌曲歌曲歌曲歌曲歌曲歌曲";}
int lv_obj_get_content_width(lv_obj_t*){return 100;}
lv_anim_t*lv_anim_get(lv_obj_t*,void*){return &animation;}
void lv_anim_delete(lv_obj_t*,void*){}
'''+s[a:b]+r'''
int main(){
 g_marquee_move_ms=1000;lv_anim_t a;a.act_time=0;assert(library_view_marquee_path(&a)==0);
 a.act_time=1800;assert(library_view_marquee_path(&a)==0);
 a.act_time=2300;assert(library_view_marquee_path(&a)==-50);
 a.act_time=2800;assert(library_view_marquee_path(&a)==-100);
 g_rows[0]={3,true,&button,&label};
 tick=599;library_view_marquee_timer(nullptr);assert(!g_marquee_label);
 tick=600;library_view_marquee_timer(nullptr);assert(g_marquee_label==&label&&label.mode==LV_LABEL_LONG_SCROLL);
 int n=changes;tick=1000;library_view_marquee_timer(nullptr);assert(changes==n);
 g_gesture.pressed=true;library_view_marquee_timer(nullptr);assert(!g_marquee_label&&label.mode==LV_LABEL_LONG_DOT);
 g_gesture.pressed=false;tick=1500;library_view_marquee_timer(nullptr);assert(!g_marquee_label);
 tick=1600;library_view_marquee_timer(nullptr);assert(g_marquee_label);
 pressure=true;library_view_marquee_timer(nullptr);assert(!g_marquee_label);
 pressure=false;tick=2300;power=99;library_view_marquee_timer(nullptr);assert(!g_marquee_label);
 power=ScreenPowerNormal;tick=3000;visible=false;library_view_marquee_timer(nullptr);assert(!g_marquee_label);
 visible=true;tick=3700;g_rows[0].center_selected=false;library_view_marquee_timer(nullptr);assert(!g_marquee_label);
 g_rows[0].center_selected=true;g_marquee_activity_tick=UINT32_MAX-299;tick=300;library_view_marquee_timer(nullptr);assert(g_marquee_label);
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'t.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-O2','-fsanitize=undefined',str(p/'t.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS actual marquee: idle delay, one selected label, no repeated restart, touch/pressure/off/hidden stop, tick wrap; UBSan')
