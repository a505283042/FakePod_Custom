from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
code=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#define FAKEPOD_PERF_OVERLAY 1
#define CONFIG_FREERTOS_USE_TRACE_FACILITY 1
#define CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS 1
#define CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER 1
struct lv_obj_t{bool hidden=false;std::string text;};struct lv_display_t{};struct lv_timer_t{};struct lv_event_t{int code;};
lv_display_t display,*g_display=&display;lv_obj_t label;lv_timer_t timer;
enum{LV_EVENT_RENDER_START,LV_EVENT_REFR_READY,LV_OBJ_FLAG_HIDDEN,LV_OBJ_FLAG_CLICKABLE,LV_OPA_COVER,LV_ALIGN_TOP_RIGHT,ScreenPowerNormal,eInvalid,pdFALSE};
int power=ScreenPowerNormal,updates=0;int64_t clock_us=1;uint32_t idle_count[2]={0,0};bool timer_ok=true;
struct TaskStatus_t{uint32_t ulRunTimeCounter;};
int xTaskGetIdleTaskHandleForCore(int c){return c;}void vTaskGetInfo(int c,TaskStatus_t*s,int,int){s->ulRunTimeCounter=idle_count[c];}
int64_t esp_timer_get_time(){return clock_us;}int screen_lock_simple_get_power(){return power;}
int lv_event_get_code(lv_event_t*e){return e->code;}
bool lv_obj_has_flag(lv_obj_t*p,int){return p->hidden;}void lv_obj_add_flag(lv_obj_t*p,int){p->hidden=true;}void lv_obj_remove_flag(lv_obj_t*p,int f){if(f==LV_OBJ_FLAG_HIDDEN)p->hidden=false;}
const char*lv_label_get_text(lv_obj_t*p){return p->text.c_str();}void lv_label_set_text(lv_obj_t*p,const char*t){p->text=t;++updates;}
lv_obj_t*lv_layer_sys(){return &label;}lv_obj_t*lv_label_create(lv_obj_t*){return &label;}void ui_common_lock_object(lv_obj_t*){}
int lv_font_default(){return 0;}uint32_t lv_color_hex(uint32_t c){return c;}
void lv_obj_set_style_text_font(lv_obj_t*,int,int){}void lv_obj_set_style_text_color(lv_obj_t*,uint32_t,int){}
void lv_obj_set_style_bg_color(lv_obj_t*,uint32_t,int){}void lv_obj_set_style_bg_opa(lv_obj_t*,int,int){}void lv_obj_set_style_pad_all(lv_obj_t*,int,int){}
void lv_obj_align(lv_obj_t*,int,int,int){}void lv_obj_delete(lv_obj_t*){}
lv_timer_t*lv_timer_create(void(*)(lv_timer_t*),int ms,void*){assert(ms==1000);return timer_ok?&timer:nullptr;}
void lv_display_add_event_cb(lv_display_t*,void(*)(lv_event_t*),int,void*){}
#include "perf_overlay.inc"
int main(){
 ui_perf_overlay_create();assert(g_perf_overlay_label);
 lv_event_t start{LV_EVENT_RENDER_START},done{LV_EVENT_REFR_READY};
 for(int i=0;i<30;++i){ui_perf_overlay_display_cb(&start);ui_perf_overlay_display_cb(&done);}
 ui_perf_overlay_display_cb(&done);assert(g_perf_overlay_frames==30);
 clock_us+=1000000;idle_count[0]+=250000;idle_count[1]+=750000;ui_perf_overlay_timer_cb(&timer);
 assert(label.text=="CPU0  75%  CPU1  25%\nUI 30.0 FPS");
 power=99;clock_us+=1000000;int n=updates;ui_perf_overlay_timer_cb(&timer);assert(label.hidden&&updates==n);
 power=ScreenPowerNormal;clock_us+=1000000;ui_perf_overlay_timer_cb(&timer);assert(!label.hidden);
 assert(fakepod_perf::busy_percent(0,1000000)==100);assert(fakepod_perf::busy_percent(2000000,1000000)==0);
 uint32_t before=UINT32_MAX-100,after=149;assert(fakepod_perf::busy_percent(after-before,1000)==75);
 g_perf_overlay_label=nullptr;timer_ok=false;ui_perf_overlay_create();assert(!g_perf_overlay_label);
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'t.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-O2','-fsanitize=undefined','-I'+str(root/'src/ui'),str(p/'t.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS actual overlay callbacks: CPU windows/wrap, rendered FPS, no empty-cycle count, screen off, timer failure; UBSan')
