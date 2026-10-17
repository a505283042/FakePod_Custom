bool player_state_select_phone_queue(PlayerMediaSource source,PlayerListType type,uint32_t group,uint32_t track,PlayerFolderScope scope,const char *folder)
{
    const bool nas=source==PlayerMediaSource::Nas;
    const uint32_t total=nas?nas_library_source_track_count():media_library_get_count();
    if(track>=total || (scope!=PlayerFolderScope::All && (!folder || !*folder)))return false;
    const uint32_t *members=nullptr;uint32_t count=total;
    if(scope==PlayerFolderScope::All && type!=PlayerListType::AllTracks){
        if(type==PlayerListType::Artist){MediaArtistGroupViewV2 v={};if(!(nas?nas_library_source_get_artist(group,&v):media_groups_v2_get_artist(group,&v)))return false;members=v.track_indices;count=v.track_count;}
        else if(type==PlayerListType::Album){MediaAlbumGroupViewV2 v={};if(!(nas?nas_library_source_get_album(group,&v):media_groups_v2_get_album(group,&v)))return false;members=v.track_indices;count=v.track_count;}
        else if(type==PlayerListType::Decade){MediaDecadeGroupViewV2 v={};if(!(nas?nas_library_source_get_decade(group,&v):media_groups_v2_get_decade(group,&v)))return false;members=v.track_indices;count=v.track_count;}
        else return false;
        if(count && !members)return false;
    }
    if(!count)return false;
    const size_t bytes=sizeof(PhoneQueue)+size_t(count-1)*sizeof(uint32_t);
    auto *q=static_cast<PhoneQueue*>(heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!q)return false;
    memset(q,0,sizeof(PhoneQueue));q->source=source;q->scope=scope;
    q->generation=nas?nas_library_source_generation():media_catalog_v2_generation();q->position=UINT32_MAX;q->context_id=++g_phone_queue_serial;
    if(folder)snprintf(q->folder,sizeof(q->folder),"%s",folder);
    for(uint32_t i=0;i<count;i++){
        const uint32_t id=members?members[i]:i;
        if(scope!=PlayerFolderScope::All){
            MediaTrackViewV2 v={};if(!(nas?nas_library_source_get_track_view(id,&v):media_catalog_v2_get_track_view(id,&v)) || !v.path)continue;
            const size_t prefix=strlen(folder);
            if(nas?strncmp(v.path,folder,prefix):strncasecmp(v.path,folder,prefix))continue;
            if(scope==PlayerFolderScope::Level2 && strchr(v.path+prefix,'/'))continue;
        }
        if(id==track)q->position=q->count;
        q->tracks[q->count++]=id;
    }
    if(q->position==UINT32_MAX){heap_caps_free(q);return false;}
    const size_t pos=type==PlayerListType::AllTracks?track:q->position;
    bool selected=false;
    if(nas)selected=nas_bind(type,group,pos);
    else if(type==PlayerListType::AllTracks)selected=player_playlist_bind_all(pos);
    else if(type==PlayerListType::Artist)selected=player_playlist_bind_artist(group,pos);
    else if(type==PlayerListType::Album)selected=player_playlist_bind_album(group,pos);
    else selected=player_playlist_bind_decade(group,pos);
    if(!selected){heap_caps_free(q);return false;}
    phone_queue_clear();g_source=source;portENTER_CRITICAL(&g_phone_queue_lock);g_phone_queue=q;portEXIT_CRITICAL(&g_phone_queue_lock);
    return player_state_select(true,"APP独立列表点歌");
}
