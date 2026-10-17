// Isolated protocol: op bit7 identifies a phone-owned browse session.
// Read-only catalog exports never reference or modify g_state/g_search/device settings.
static uint32_t lri_count(bool nas,uint32_t section) {
    switch(section){
        case 0:return nas?nas_library_source_track_count():media_library_get_count();
        case 1:return nas?nas_library_source_artist_count():media_groups_v2_artist_count();
        case 2:return nas?nas_library_source_album_count():media_groups_v2_album_count();
        case 3:return nas?nas_library_source_decade_count():media_groups_v2_decade_count();
        default:return 0;
    }
}
static uint32_t lri_generation(bool nas){return nas?nas_library_source_generation():media_catalog_v2_generation();}
static uint32_t lri_identity(bool nas){const auto c=media_catalog_v2_current();return nas?(nas_library_source_generation()^0x80000000U):(c?c->source_crc32:0);}
static bool lri_ready(bool nas){
    bool loading;taskENTER_CRITICAL(&g_nas_load_lock);loading=g_nas_load_running;taskEXIT_CRITICAL(&g_nas_load_lock);
    return !loading && !g_nas_index_reload_active && !nas_catalog_service_is_server_switching() &&
        !usb_storage_service_blocks_normal_runtime() && (nas?nas_library_source_ready():media_catalog_v2_ready());
}
static void lri_prepare_nas() {
    if(g_nas_remote_retry_due && int32_t(xTaskGetTickCount()-g_nas_remote_retry_due)<0)return;
    if(nas_library_source_ready() || g_nas_index_reload_active || nas_catalog_service_is_server_switching() || usb_storage_service_blocks_normal_runtime())return;
    taskENTER_CRITICAL(&g_nas_load_lock);
    if(g_nas_load_running){taskEXIT_CRITICAL(&g_nas_load_lock);return;}
    g_nas_load_running=true;g_nas_load_remote_only=true;g_nas_load_show=false;
    g_nas_load_scope=DeviceMusicListScope::All;g_nas_load_result=ESP_OK;
    taskEXIT_CRITICAL(&g_nas_load_lock);
    TaskHandle_t task=nullptr;
    if(xTaskCreatePinnedToCoreWithCaps(library_view_nas_load_task,"nas_phone_cache",NAS_LIBRARY_LOAD_TASK_STACK_BYTES,nullptr,
        NAS_LIBRARY_LOAD_TASK_PRIORITY,&task,NAS_LIBRARY_LOAD_TASK_CORE,NAS_LIBRARY_LOAD_TASK_STACK_CAPS)!=pdPASS){
        taskENTER_CRITICAL(&g_nas_load_lock);g_nas_load_running=false;g_nas_load_remote_only=false;g_nas_load_result=ESP_ERR_NO_MEM;g_nas_remote_retry_due=xTaskGetTickCount()+pdMS_TO_TICKS(30000);taskEXIT_CRITICAL(&g_nas_load_lock);
    }
}
static void lri_state(LrWriter &w,bool nas) {
    const bool ready=lri_ready(nas);
    w.u8(nas?1:0);w.u8(0);w.u8(0);w.u8(0);w.u8(0);w.u8(0);w.u8(ready);w.u8(lr_music());
    w.u32(ready?lri_generation(nas):0);w.u32(ready?lri_identity(nas):0);w.u32(1);
    AudioStateSnapshot audio={};
    const bool playing=audio_service_get_snapshot(&audio) && audio.ready &&
        !video_app_remote_control_active() && !visual_music_app_remote_control_active();
    w.u32(playing?audio.track_index:UINT32_MAX);
    w.u32(playing && audio.source==AudioPlaybackSource::NasHttp?1:0);w.u32(UINT32_MAX);
    for(uint32_t i=0;i<4;i++)w.u32(ready?lri_count(nas,i):0);
    w.u32(ready?lri_count(nas,0):0);
}
static bool lri_play(bool nas,uint32_t track,uint32_t type,uint32_t group) {
    MediaTrackViewV2 view={};
    if(!(nas?nas_library_source_get_track_view(track,&view):media_catalog_v2_get_track_view(track,&view)) || !view.row)return false;
    if(nas && view.row->format!=MediaFormat::MP3 && view.row->format!=MediaFormat::FLAC)return false;
    uint32_t position=track;
    if(type>=1 && type<=3){
        const uint32_t *ids=nullptr;uint32_t count=0;
        if(type==1){MediaArtistGroupViewV2 g={};if(!(nas?nas_library_source_get_artist(group,&g):media_groups_v2_get_artist(group,&g)))return false;ids=g.track_indices;count=g.track_count;}
        if(type==2){MediaAlbumGroupViewV2 g={};if(!(nas?nas_library_source_get_album(group,&g):media_groups_v2_get_album(group,&g)))return false;ids=g.track_indices;count=g.track_count;}
        if(type==3){MediaDecadeGroupViewV2 g={};if(!(nas?nas_library_source_get_decade(group,&g):media_groups_v2_get_decade(group,&g)))return false;ids=g.track_indices;count=g.track_count;}
        position=UINT32_MAX;for(uint32_t i=0;ids && i<count;i++)if(ids[i]==track){position=i;break;}
        if(position==UINT32_MAX)return false;
    } else if(type>5)return false;
    // Resolve and validate directory identity from the authoritative track path.
    char *folder=lr_scratch->path;folder[0]=0;
    if(type>=4){
        const char *root=nas?"/":"/sdcard/MUSIC/";const size_t n=strlen(root);
        if(!view.path || (nas?strncmp(view.path,root,n):strncasecmp(view.path,root,n)))return false;
        const char *end=view.path+n;
        for(uint32_t level=0;level<type-3;level++){end=strchr(end,'/');if(!end)return false;++end;}
        const size_t length=size_t(end-view.path);if(length>=PLAYER_FOLDER_PATH_MAX)return false;
        memcpy(folder,view.path,length);folder[length]=0;
        const auto scope=PlayerFolderScope(type-3);
        if(!(nas?nas_library_source_folder_selection_available(scope,folder,nullptr):player_playlist_folder_selection_available(scope,folder,nullptr)))return false;
    }
    if(!player_control_select_phone_queue(nas?1:0,type<4?uint8_t(type):0,group,track,type<4?0:uint8_t(type-3),folder))return false;
    // Browsing does not touch the playback queue. A song click explicitly binds it.
    return player_control_play_current();
}
static void library_remote_process_independent(const uint8_t *q) {
    const uint8_t op=q[2]&0x7f;const bool probe=q[3]==255 && op==0;
    bool nas=q[3]==1;
    // Logical library source is distinct from the source of an already playing song.
    if(probe)nas=device_settings_music_library_source()==DeviceMusicLibrarySource::Nas;
    const uint32_t section=lr_u32(q+16);
    lr_result=0;
    if(op==6)g_nas_remote_retry_due=0;
    if(q[3]>1 && !probe)lr_result=3;
    else if(op==9){
        const auto target=nas?DeviceMusicLibrarySource::Nas:DeviceMusicLibrarySource::Local;
        if(!lr_music() || usb_storage_service_blocks_normal_runtime() || nas_catalog_service_is_server_switching() ||
            (target!=device_settings_music_library_source() && device_settings_set_music_library_source(target)!=ESP_OK))lr_result=4;
        else library_view_on_library_source_changed(target);
    }
    else if(!probe && (op==0 || op==6) && nas && !lri_ready(true))lri_prepare_nas();
    if(!lr_result && !probe && op!=9 && !lri_ready(nas))lr_result=1;
    if(!lr_result && op!=0 && op!=6 && op!=9 && (lr_u32(q+8)!=lri_generation(nas) || lr_u32(q+28)!=lri_identity(nas)))lr_result=2;
    if(!lr_result && op==8){
        if(!lr_music() || !lri_play(nas,section,lr_u32(q+20),lr_u32(q+24)))lr_result=4;
    } else if(op!=0 && op!=1 && op!=6 && op!=8 && op!=9 && !lr_result)lr_result=3;
    LrWriter w{lr_reply,LR_CAP};lri_state(w,nas);
    if(!lr_result && op==1){
        uint32_t rec=lr_u32(q+20),off=lr_u32(q+24);const uint32_t count=lri_count(nas,section);
        if(section>3 || rec>count || (rec==count && off))lr_result=3;
        else {
            const size_t header=w.used;w.u32(0);w.u32(0);w.u8(0);
            while(rec<count && w.used<LR_CAP){
                LrWriter part{lr_reply+w.used,LR_CAP-w.used,0,0,off};
                if(!lr_record(section,rec,part,true,nas) || off>part.total){lr_result=3;break;}
                w.used+=part.used;
                if(off+part.used==part.total){++rec;off=0;}else{off+=part.used;break;}
            }
            lr_put(lr_reply+header,rec);lr_put(lr_reply+header+4,off);lr_reply[header+8]=rec==count;
        }
    }
    lr_size=w.used;lr_sent=0;lr_crc=lr_crc32(lr_reply,lr_size);
}
