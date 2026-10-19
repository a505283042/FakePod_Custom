from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2];s=(root/'src/ui/screens/library_view.cpp').read_text()
a=s.index('static void library_view_row_push_finish(lv_timer_t *)\n');b=s.index('\nstatic void library_view_scrollbar_set_visible',a)
code=r'''
#include <cassert>
void library_view_marquee_stop(){}
#include <cstdlib>
struct lv_timer_t{};lv_timer_t timer,*g_row_push_timer=&timer;
int g_row_push_offset=0,g_manual_scroll_y=0,paints=0;bool g_row_push_pending=false,visible=true,pressure=false,paused=true;
struct FlacStorageWindowSnapshot{bool active=true,pressure_active=false;};
bool flac_decoder_get_storage_window(FlacStorageWindowSnapshot*p){p->pressure_active=pressure;return true;}
bool library_view_is_visible(){return visible;}int library_abs(int x){return abs(x);}
int library_view_row_step(){return 70;}int library_view_clamp_scroll_y(int y){if(y<0)return 0;if(y>700)return 700;return y/70*70;}
void library_view_refresh_virtual_rows(bool){++paints;}void library_view_scrollbar_update_position(){}
void lv_timer_pause(lv_timer_t*){paused=true;}void lv_timer_reset(lv_timer_t*){}void lv_timer_resume(lv_timer_t*){paused=false;}
'''+s[a:b]+r'''
int main(){
 library_view_set_scroll_y(70);assert(paints==1&&g_row_push_offset==35&&g_row_push_pending&&!paused);
 library_view_set_scroll_y(140);library_view_set_scroll_y(210);assert(paints==1&&g_manual_scroll_y==210);
 library_view_row_push_finish(nullptr);assert(paints==2&&g_row_push_offset==0&&!g_row_push_pending&&paused);
 library_view_set_scroll_y(140);assert(g_row_push_offset==-35);
 pressure=true;library_view_set_scroll_y(70);assert(g_row_push_offset==0&&!g_row_push_pending&&paused);
 pressure=false;library_view_set_scroll_y(350);assert(!g_row_push_pending); // big jump has no animation
 g_row_push_timer=nullptr;library_view_set_scroll_y(420);assert(!g_row_push_pending);
 g_row_push_timer=&timer;library_view_set_scroll_y(490);visible=false;int n=paints;
 library_view_row_push_finish(nullptr);assert(paints==n&&!g_row_push_pending&&g_row_push_offset==0);
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'t.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-O2','-fsanitize=undefined',str(p/'t.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS actual row-push setter/finish: two paints, coalescing, reverse, pressure, jumps, timer fallback, hidden; UBSan')
