// One playback queue captured by an explicit phone song click. Device browse scope
// and folder selections remain untouched. Native playlist still owns the track binding.
struct PhoneQueue {
    PlayerMediaSource source;uint32_t generation,count,position,context_id;PlayerFolderScope scope;
    char folder[PLAYER_FOLDER_PATH_MAX];uint32_t tracks[1];
};
static portMUX_TYPE g_phone_queue_lock=portMUX_INITIALIZER_UNLOCKED;
static PhoneQueue *g_phone_queue=nullptr;
static uint32_t g_phone_queue_serial=0;
static void phone_queue_clear(){portENTER_CRITICAL(&g_phone_queue_lock);auto *old=g_phone_queue;g_phone_queue=nullptr;portEXIT_CRITICAL(&g_phone_queue_lock);if(old)heap_caps_free(old);}
static bool phone_queue_valid(){return g_phone_queue && g_phone_queue->source==g_source && g_phone_queue->generation==
    (g_source==PlayerMediaSource::Nas?nas_library_source_generation():media_catalog_v2_generation());}

// An invalid captured queue must fail closed, never fall back to the device browser.
static bool phone_queue_exists(){
    portENTER_CRITICAL(&g_phone_queue_lock);const bool exists=g_phone_queue!=nullptr;portEXIT_CRITICAL(&g_phone_queue_lock);
    return exists;
}
