from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2];s=(root/'src/ui/screens/library_view.cpp').read_text()
a=s.index('static void library_view_set_row_text(');b=s.index('static void library_view_set_folder_row_text',a)
code=r'''
#include <cassert>
#include <cstdint>
struct Obj{int y=0,h=0;};struct LibraryVirtualRow{Obj*label;Obj*meta;};
struct{bool active=false;}g_search;int library_view_row_h(){return g_search.active?44:62;}
void lv_obj_set_y(Obj*p,int v){p->y=v;}void lv_obj_set_height(Obj*p,int v){p->h=v;}
constexpr int LV_OBJ_FLAG_HIDDEN=1;void lv_obj_add_flag(Obj*,int){}
void lv_label_set_text_fmt(Obj*,const char*,const char*,const char*){}void lv_label_set_text(Obj*,const char*){}
'''+s[a:b]+r'''
int main(){Obj label,meta;LibraryVirtualRow row{&label,&meta};
 for(bool search:{false,true,false,true}){g_search.active=search;library_view_set_row_text(row,"title","artist");
 assert(label.y*2+label.h==library_view_row_h());assert(label.h==(search?30:32));}
}
'''
code=code.replace('#include <cassert>','#include <cassert>\n#include <initializer_list>')
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'t.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-O2','-fsanitize=undefined',str(p/'t.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS actual text binder: normal/search/rebind centered; UBSan')
