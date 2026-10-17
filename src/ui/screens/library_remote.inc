// R146 APP Library v1. Included here to reuse the device's private browse model.
// Only the LVGL timer touches Catalog views. TCP sees bounded copied bytes only.
#include <new>
#include "video_app.h"
#include "visual_music_app.h"

namespace {
constexpr size_t LR_CAP = 2048;
static portMUX_TYPE lr_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t lr_request[32];
// UI-thread-only scratch: large path/selection temporaries must not stack on
// top of folder persistence and NVS inside the LVGL timer callback.
struct LrScratch { DeviceMusicListSelection selection; char path[PLAYER_FOLDER_PATH_MAX];
    char parent[PLAYER_FOLDER_PATH_MAX]; char name[PLAYER_FOLDER_PATH_MAX]; };
static LrScratch *lr_scratch = nullptr;
static uint8_t *lr_reply = nullptr; // exactly 2KB PSRAM, allocated only when used
static uint8_t lr_phase = 0; // idle, pending, processing, ready
static bool lr_cancelled = false;
static uint16_t lr_size = 0, lr_sent = 0;
static uint32_t lr_crc = 0;
static uint8_t lr_result = 0;

static uint32_t lr_u32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
static void lr_put(uint8_t *p, uint32_t v) {
    for (unsigned i=0;i<4;++i) p[i]=uint8_t(v >> (8*i));
}
static uint32_t lr_hash(uint32_t h, const void *p, size_t n) {
    auto b=static_cast<const uint8_t *>(p);
    while(n--) h=(h ^ *b++)*16777619U;
    return h;
}
// CRC-32/ISO-HDLC for each complete reply (including the state prefix).
static uint32_t lr_crc32(const uint8_t *p, size_t n) {
    uint32_t c=~0U;
    while(n--) { c^=*p++; for(unsigned i=0;i<8;++i) c=(c>>1)^((c&1)?0xEDB88320U:0); }
    return ~c;
}
struct LrWriter {
    uint8_t *out; size_t cap, used=0, total=0, skip=0;
    void bytes(const void *p, size_t n) {
        size_t off=skip>total?skip-total:0;
        if(off<n && used<cap) { size_t take=n-off; if(take>cap-used) take=cap-used;
            memcpy(out+used, static_cast<const uint8_t *>(p)+off,take); used+=take; }
        total+=n;
    }
    void u8(uint8_t n) { bytes(&n,1); }
    void u32(uint32_t n) { uint8_t b[4];lr_put(b,n);bytes(b,4); }
    void str(const char *s) { size_t n=s?strlen(s):0;u32(n);if(n)bytes(s,n); }
};
static bool lr_available() {
    bool loading;
    taskENTER_CRITICAL(&g_nas_load_lock); loading=g_nas_load_running;taskEXIT_CRITICAL(&g_nas_load_lock);
    return g_root && !loading && !g_nas_index_reload_active &&
        !nas_catalog_service_is_server_switching() && !usb_storage_service_blocks_normal_runtime() &&
        (library_source_is_nas()?nas_library_source_ready():media_catalog_v2_ready());
}
static bool lr_music() {
    return app_manager_foreground()==AppId::Music &&
        !video_app_remote_control_active() && !visual_music_app_remote_control_active();
}
static uint32_t lr_identity() {
    if(library_source_is_nas()) return nas_library_source_generation()^0x80000000U;
    const auto c=media_catalog_v2_current();return c?c->source_crc32:0;
}
static uint32_t lr_revision() {
    uint32_t values[]={uint32_t(g_source),library_source_generation(),uint32_t(library_view_active_folder_scope()),
        uint32_t(g_state.mode),uint32_t(g_state.detail_type),g_state.detail_group_index,
        uint32_t(g_state.folder_view),uint32_t(g_search.active),g_search.query_length,g_search.key_index,
        library_view_item_count()};
    uint32_t h=lr_hash(2166136261U,values,sizeof(values));
    h=lr_hash(h,g_state.level2_parent_path,strlen(g_state.level2_parent_path));
    h=lr_hash(h,g_search.query,g_search.query_length);
    char path[PLAYER_FOLDER_PATH_MAX]={};
    if(library_source_copy_folder_selection(library_view_active_folder_scope(),path,sizeof(path)))
        h=lr_hash(h,path,strlen(path));
    return h?h:1;
}
static uint32_t lr_count(uint32_t section) {
    switch(section) {
        case 0:return library_source_is_nas()?nas_library_source_track_count():media_library_get_count();
        case 1:return library_source_artist_count();case 2:return library_source_album_count();
        case 3:return library_source_decade_count();case 4:return library_view_item_count();default:return 0;
    }
}
// Resolve a displayed row without constructing LVGL objects. Positions remain the
// unfiltered device positions, so phone searches cannot silently create a queue.
static bool lr_binding(uint32_t display, LibraryRowBinding &b, char *name, size_t size, uint32_t &count) {
    if(display>=library_view_item_count())return false;
    uint32_t i=library_view_source_item_index(display);
    b={};b.generation=library_source_generation();count=0;name[0]=0;
    if(library_view_folder_browser_active()) {
        if(g_state.folder_view==LibraryFolderView::Level2Folders && i==0) {
            b.action=LibraryRowAction::FolderUp;snprintf(name,size,"上一级");return true;
        }
        size_t pos=i;
        if(g_state.folder_view==LibraryFolderView::Level2Parents) {
            if(!library_view_level2_parent_at(i,&pos,name,size,&count))return false;
            b.action=LibraryRowAction::SelectLevel2Parent;
        } else {
            bool l2=g_state.folder_view==LibraryFolderView::Level2Folders;
            if(l2 && !library_view_resolve_level2_parent())return false;
            if(l2)pos=i-1;
            if(!library_source_copy_folder_option_at(l2?PlayerFolderScope::Level2:PlayerFolderScope::Level1,
                l2?g_state.level2_parent_path:nullptr,pos,name,size,&count))return false;
            b.action=l2?LibraryRowAction::SelectLevel2Folder:LibraryRowAction::SelectLevel1Folder;
        }
        b.position=pos;
        const char *leaf=library_folder_leaf_in_place(name);
        if(leaf && leaf!=name)memmove(name,leaf,strlen(leaf)+1);
        return true;
    }
    if(g_state.mode==LibraryBrowseMode::AllTracks || g_state.mode==LibraryBrowseMode::GroupTracks) {
        const bool detail=g_state.mode==LibraryBrowseMode::GroupTracks;
        if(!(detail?library_view_detail_track_index(i,&b.track_index,nullptr):library_view_top_track_index(i,&b.track_index)))return false;
        b.position=i;b.group_type=detail?g_state.detail_type:PlayerListType::AllTracks;
        b.group_index=detail?g_state.detail_group_index:UINT32_MAX;
        b.action=detail?LibraryRowAction::PlayGroupTrack:
            (library_view_folder_scope_active()?LibraryRowAction::PlayFolderTrack:LibraryRowAction::PlayAllTrack);
        return true;
    }
    b.group_index=i;
    if(g_state.mode==LibraryBrowseMode::Artists) { b.action=LibraryRowAction::OpenArtist;b.group_type=PlayerListType::Artist; }
    else if(g_state.mode==LibraryBrowseMode::Albums) { b.action=LibraryRowAction::OpenAlbum;b.group_type=PlayerListType::Album; }
    else if(g_state.mode==LibraryBrowseMode::Decades) { b.action=LibraryRowAction::OpenDecade;b.group_type=PlayerListType::Decade; }
    else return false;
    return true;
}
static bool lr_group(uint32_t type,uint32_t i,LrWriter &w,bool independent=false,bool nas=false) {
    const char *name="",*artist=""; const uint32_t *ids=nullptr;uint32_t count=0,decade=0;
    char decade_name[32]={};
    if(type==1) { MediaArtistGroupViewV2 v={};if(!(independent?(nas?nas_library_source_get_artist(i,&v):media_groups_v2_get_artist(i,&v)):library_source_get_artist(i,&v)))return false;name=v.name;ids=v.track_indices;count=v.track_count; }
    else if(type==2) { MediaAlbumGroupViewV2 v={};if(!(independent?(nas?nas_library_source_get_album(i,&v):media_groups_v2_get_album(i,&v)):library_source_get_album(i,&v)))return false;name=v.title;artist=v.artist;ids=v.track_indices;count=v.track_count; }
    else if(type==3) { MediaDecadeGroupViewV2 v={};if(!(independent?(nas?nas_library_source_get_decade(i,&v):media_groups_v2_get_decade(i,&v)):library_source_get_decade(i,&v)))return false;
        decade=v.unknown?0:v.decade_start; snprintf(decade_name,sizeof(decade_name),v.unknown?"未知年代":"%u年代",v.decade_start);
        name=decade_name;ids=v.track_indices;count=v.track_count; }
    else return false;
    if(count && !ids)return false;
    w.str(name);w.str(artist);w.u32(decade);w.u32(count);
    for(uint32_t j=0;j<count;++j)w.u32(ids[j]);
    return true;
}
static bool lr_record(uint32_t section,uint32_t i,LrWriter &w,bool independent=false,bool nas=false) {
    w.u32(i);
    if(section==0) {
        MediaTrackViewV2 v={};if(!(independent?(nas?nas_library_source_get_track_view(i,&v):media_catalog_v2_get_track_view(i,&v)):library_source_get_track_view(i,&v))||!v.row)return false;
        w.str(v.title);w.str(v.artist);w.str(v.album);w.str(v.path);
        w.u32(v.row->metadata_flags);w.u32(v.row->release_year);w.u32(v.row->original_year);w.u32(uint32_t(v.row->format));
        return true;
    }
    if(section<4)return lr_group(section,i,w,independent,nas);
    LibraryRowBinding b={};char name[PLAYER_FOLDER_PATH_MAX]={};uint32_t count=0;
    if(!lr_binding(i,b,name,sizeof(name),count))return false;
    w.u8(uint8_t(b.action));w.u8(uint8_t(b.group_type));w.u32(b.position);w.u32(b.group_index);w.u32(b.track_index);
    if(b.track_index!=UINT32_MAX) { MediaTrackViewV2 v={};if(!library_source_get_track_view(b.track_index,&v))return false;w.str(v.title);w.str(v.artist); }
    else if(b.group_index!=UINT32_MAX) {
        const char *title="",*sub="";char dec[32]={};
        if(b.group_type==PlayerListType::Artist) { MediaArtistGroupViewV2 v={};if(!library_source_get_artist(b.group_index,&v))return false;title=v.name;count=v.track_count; }
        else if(b.group_type==PlayerListType::Album) { MediaAlbumGroupViewV2 v={};if(!library_source_get_album(b.group_index,&v))return false;title=v.title;sub=v.artist;count=v.track_count; }
        else { MediaDecadeGroupViewV2 v={};if(!library_source_get_decade(b.group_index,&v))return false;snprintf(dec,sizeof(dec),v.unknown?"未知年代":"%u年代",v.decade_start);title=dec;count=v.track_count; }
        w.str(title);w.str(sub);
    } else { w.str(name);w.str(""); }
    w.u32(count);return true;
}
static void lr_state(LrWriter &w) {
    bool ready=lr_available();
    w.u8(uint8_t(g_source));w.u8(uint8_t(library_view_active_folder_scope()));
    w.u8(uint8_t(g_state.mode));w.u8(uint8_t(g_state.detail_type));w.u8(uint8_t(g_state.folder_view));
    w.u8(g_search.active);w.u8(ready);w.u8(lr_music());
    w.u32(ready?library_source_generation():0);w.u32(ready?lr_identity():0);w.u32(ready?lr_revision():0);
    w.u32(player_state_is_ready()?player_state_get_index():UINT32_MAX);
    w.u32(uint32_t(player_state_get_source()));w.u32(g_state.detail_group_index);
    for(uint32_t j=0;j<5;++j)w.u32(ready?lr_count(j):0);
}
static void lr_clear_search() {
    if(g_search.active)library_search_exit();
    library_view_inertia_stop(false);
}
#include "library_remote_independent.inc"

static void library_remote_process(const uint8_t *q) {
    if(q[2]&0x80U){library_remote_process_independent(q);return;}
    uint8_t op=q[2],src=q[3];uint32_t gen=lr_u32(q+8),rev=lr_u32(q+12),a=lr_u32(q+16);
    lr_result=0;
    if(op==6) {
        bool loading;
        taskENTER_CRITICAL(&g_nas_load_lock);loading=g_nas_load_running;taskEXIT_CRITICAL(&g_nas_load_lock);
        if(loading || g_nas_index_reload_active || nas_catalog_service_is_server_switching() || usb_storage_service_blocks_normal_runtime() || !g_root)lr_result=1;
        else if(!lr_music() || (src>1 && src!=255))lr_result=4;
        else {
            auto target=src==255?device_settings_music_library_source():DeviceMusicLibrarySource(src);
            const bool show=library_view_is_visible();
            const bool alreadyBound=uint8_t(target)==uint8_t(g_source) && lr_available();
            if(alreadyBound) {
                // Keep the existing classification, search and scroll state.
            } else if(target==DeviceMusicLibrarySource::Nas) {
                auto &s=lr_scratch->selection;s={};device_settings_get_music_list_selection_for_source(target,&s);
                if(!library_view_open_nas(s.scope,show))lr_result=4;
            } else library_view_open(show);
            // Save source only after opening was accepted. No playback is started.
            if(!lr_result && src!=255) {
                if(device_settings_set_music_library_source(target)!=ESP_OK)lr_result=4;
                else library_view_on_library_source_changed(target);
            }
        }
    } else if(!lr_available())lr_result=1;
    else if(src!=255 && src!=uint8_t(g_source))lr_result=2;
    else if(op!=0 && (gen!=library_source_generation() || ((op!=1 || a==4) && rev!=lr_revision())))lr_result=2;
    else if(op>=2 && op<=6 && !lr_music())lr_result=4;
    else if(op==2) {
        if(a>2)lr_result=3;
        else {
            auto scope=PlayerFolderScope(a);
            bool ok=false;
            if(a==0) {
                auto &s=lr_scratch->selection;s={};device_settings_get_music_list_selection_for_source(library_device_source(),&s);
                ok=device_settings_set_music_list_selection_for_source(library_device_source(),DeviceMusicListScope::All,s.level1_path,s.level2_path)==ESP_OK;
                if(ok)ok=library_source_is_nas()?nas_library_source_set_folder_scope(scope):player_playlist_set_folder_scope(scope);
            } else {
                char *path=lr_scratch->path;memset(path,0,PLAYER_FOLDER_PATH_MAX);
                bool found=library_source_copy_folder_selection(scope,path,PLAYER_FOLDER_PATH_MAX) &&
                    library_source_folder_selection_available(scope,path,nullptr);
                if(!found && a==1)found=library_source_copy_folder_option_at(scope,nullptr,0,path,PLAYER_FOLDER_PATH_MAX,nullptr);
                if(!found && a==2) {
                    char *parent=lr_scratch->parent;memset(parent,0,PLAYER_FOLDER_PATH_MAX);size_t parent_pos=0;
                    found=library_view_level2_parent_at(0,&parent_pos,parent,PLAYER_FOLDER_PATH_MAX,nullptr) &&
                        library_source_copy_folder_option_at(scope,parent,0,path,PLAYER_FOLDER_PATH_MAX,nullptr);
                }
                ok=found && library_view_persist_folder_selection(scope,path);
            }
            if(!ok)lr_result=4;
            else {
                lr_clear_search();g_state.mode=LibraryBrowseMode::AllTracks;g_state.detail_group_index=UINT32_MAX;
                g_state.folder_view=a==1?LibraryFolderView::Level1Folders:(a==2?LibraryFolderView::Level2Parents:LibraryFolderView::Tracks);
                g_state.top_scroll_y[0]=0;library_view_schedule_render();
                if(player_state_get_source()==(library_source_is_nas()?PlayerMediaSource::Nas:PlayerMediaSource::Local))cassette_view_on_playback_queue_changed();
            }
        }
    } else if(op==3) {
        if(a>3 || library_view_active_folder_scope()!=PlayerFolderScope::All)lr_result=3;
        else { lr_clear_search();g_state.mode=LibraryBrowseMode(a);g_state.detail_group_index=UINT32_MAX;library_view_schedule_render(); }
    } else if(op==4) {
        LibraryRowBinding b={};char *name=lr_scratch->name;memset(name,0,PLAYER_FOLDER_PATH_MAX);uint32_t count=0;
        if(!lr_binding(a,b,name,PLAYER_FOLDER_PATH_MAX,count)||b.track_index!=lr_u32(q+20))lr_result=2;
        else if(!library_view_activate_binding(&b))lr_result=4;
    } else if(op==5) {
        if(g_search.active)library_search_exit();
        else if(g_state.mode==LibraryBrowseMode::GroupTracks)library_view_return_to_parent_category();
        else if(library_view_active_folder_scope()!=PlayerFolderScope::All) {
            g_state.folder_view=library_view_active_folder_scope()==PlayerFolderScope::Level1?LibraryFolderView::Level1Folders:LibraryFolderView::Level2Parents;
            library_view_schedule_render();
        }
    } else if(op!=0 && op!=1 && op!=7)lr_result=3;

    LrWriter w{lr_reply,LR_CAP};lr_state(w);
    if((op==1 || op==7) && !lr_result) {
        uint32_t rec=lr_u32(q+20),off=lr_u32(q+24),count=lr_count(a);
        if(a>4 || rec>count || (rec==count && off) ||
            (op==7 && (a!=4 || library_view_folder_browser_active() ||
                (g_state.mode!=LibraryBrowseMode::AllTracks && g_state.mode!=LibraryBrowseMode::GroupTracks))))lr_result=3;
        else {
            // Reserve cursor fields. A record may span pages, including long UTF-8 tags.
            size_t header=w.used;w.u32(0);w.u32(0);w.u8(0);
            while(rec<count && w.used<LR_CAP) {
                LrWriter part{lr_reply+w.used,LR_CAP-w.used,0,0,off};
                bool valid;
                if(op==7) {
                    LibraryRowBinding binding={};uint32_t ignored=0;
                    valid=lr_binding(rec,binding,lr_scratch->name,PLAYER_FOLDER_PATH_MAX,ignored) && binding.track_index!=UINT32_MAX;
                    if(valid){part.u32(binding.position);part.u32(binding.track_index);}
                } else valid=lr_record(a,rec,part);
                if(!valid || off>part.total){lr_result=3;break;}
                w.used+=part.used;
                if(off+part.used==part.total){++rec;off=0;}else {off+=part.used;break;}
            }
            lr_put(lr_reply+header,rec);lr_put(lr_reply+header+4,off);lr_reply[header+8]=rec==count;
        }
    }
    lr_size=w.used;lr_sent=0;lr_crc=lr_crc32(lr_reply,lr_size);
}
} // namespace

bool library_view_remote_submit(const uint8_t *request,size_t size) {
    if(!request || size!=32 || request[0]!=0x30 || request[1]!=1)return false;
    // Single TCP producer; reset never frees the buffer while LVGL may be writing.
    if(!lr_reply)lr_reply=static_cast<uint8_t *>(heap_caps_malloc(LR_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!lr_scratch) {
        void *memory=heap_caps_malloc(sizeof(LrScratch),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(memory)lr_scratch=new(memory)LrScratch{};
    }
    if(!lr_reply || !lr_scratch)return false;
    bool ok=false;
    portENTER_CRITICAL(&lr_lock);
    if(lr_phase==0){memcpy(lr_request,request,32);lr_cancelled=false;lr_phase=1;ok=true;}
    portEXIT_CRITICAL(&lr_lock);return ok;
}
void library_view_remote_reset() {
    portENTER_CRITICAL(&lr_lock);
    if(lr_phase==2 || lr_phase==4)lr_cancelled=true;else lr_phase=0;
    portEXIT_CRITICAL(&lr_lock);
}
static void library_remote_timer(lv_timer_t *) {
    if (g_playback_focus_pending.exchange(false)) library_view_apply_playback_focus();
    // Wi-Fi shutdown persists Local as the browse source. If the NAS list is
    // currently visible, switch the shared UI to the local catalog on the LVGL thread.
    if (library_view_is_nas_source() && library_view_is_visible() &&
        device_settings_music_library_source() == DeviceMusicLibrarySource::Local) {
        WifiServiceSnapshot wifi = {};
        if (wifi_service_get_snapshot(&wifi) && !wifi.connected && !wifi.transition_running) {
            library_view_close_nas();
            library_view_open();
            ESP_LOGI("曲库界面", "Wi-Fi关闭后NAS列表已切换到本地曲库");
        }
    }
    uint8_t q[32];bool work=false;
    portENTER_CRITICAL(&lr_lock);
    if(lr_phase==1){memcpy(q,lr_request,32);lr_phase=2;work=true;}
    portEXIT_CRITICAL(&lr_lock);
    if(!work)return;
    library_remote_process(q);
    portENTER_CRITICAL(&lr_lock);lr_phase=lr_cancelled?0:3;portEXIT_CRITICAL(&lr_lock);
}
bool library_view_remote_take_frame(uint8_t *frame,size_t *size) {
    // Never access PSRAM while holding a spinlock: NVS/Flash operations can
    // disable caches on the other core and need interrupts to complete IPC.
    uint16_t total=0,sent=0;uint32_t crc=0;uint8_t result=0,op=0,id[4]={};
    bool ready=false;
    portENTER_CRITICAL(&lr_lock);
    if(lr_phase==3) {
        lr_phase=4;ready=true;total=lr_size;sent=lr_sent;crc=lr_crc;
        result=lr_result;op=lr_request[2];memcpy(id,lr_request+4,4);
    }
    portEXIT_CRITICAL(&lr_lock);
    if(!ready)return false;
    size_t n=total-sent;if(n>239)n=239;
    frame[0]=6;frame[1]=1;frame[2]=result;frame[3]=op;
    memcpy(frame+4,id,4);frame[8]=total&255;frame[9]=total>>8;
    frame[10]=sent&255;frame[11]=sent>>8;lr_put(frame+12,crc);
    memcpy(frame+16,lr_reply+sent,n);
    bool ok;
    portENTER_CRITICAL(&lr_lock);
    ok=!lr_cancelled;
    if(ok){lr_sent=sent+n;lr_phase=lr_sent==lr_size?0:3;}
    else lr_phase=0;
    portEXIT_CRITICAL(&lr_lock);
    if(ok)*size=n+16;
    return ok;
}

bool library_view_remote_pending() {
    bool pending;
    portENTER_CRITICAL(&lr_lock);pending=lr_phase!=0;portEXIT_CRITICAL(&lr_lock);
    return pending;
}
