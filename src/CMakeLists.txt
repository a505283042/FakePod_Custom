idf_component_register(
    SRCS
        "main.cpp"

        "boot/boot_state.cpp"

        "app/app_manager.cpp"
        "app/app_launcher_overlay.cpp"
        "app/music_app_adapter.cpp"
        "app/ebook_app.cpp"
        "app/ebook_bookmark_store.cpp"
        "app/ebook_page_index_cache.cpp"
        "app/ebook_reader_model.cpp"
        "app/visual_music_app.cpp"
        "app/visual_music_browser_model.cpp"
        "app/visual_music_midi.cpp"
        "app/visual_music_nsf.cpp"
        "app/video_app.cpp"
        "app/video_browser_model.cpp"
        "app/video_media_io.cpp"
        "app/video_benchmark.cpp"
        "app/video_probe.cpp"

        "system/system_loop.cpp"
        "system/system_runtime.cpp"
        "system/persistent_state.cpp"
        "system/power_service.cpp"
        "system/gpio0_service.cpp"
        "system/screen_lock_simple.cpp"

        "drivers/i2c/i2c_bus.cpp"

        "drivers/touch/cst820.cpp"

        "drivers/imu/qmi8658.cpp"

        "drivers/storage/sdcard.cpp"
        "drivers/storage/storage_io.cpp"

        "drivers/display/display.cpp"
        "drivers/display/display_bounded_spi.cpp"

        "drivers/audio/cs43131.cpp"
        "drivers/audio/i2s_output.c"

        "audio/audio_service.cpp"
        "audio/audio_decode_workspace.cpp"
        "audio/midi_synth.cpp"
        "audio/nsf_synth.cpp"
        "audio/audio_spectrum_snapshot.cpp"
        "audio/sources/audio_source.cpp"
        "audio/sources/sd_file_audio_source.cpp"
        "audio/sources/buffered_sd_audio_source.cpp"
        "audio/sources/avi_mp3_audio_source.cpp"
        "audio/decoders/pcm_decoder.cpp"
        "audio/decoders/wav_decoder.cpp"
        "audio/decoders/flac_decoder.cpp"
        "audio/decoders/mp3_decoder.cpp"

        "assets/artwork_loader.cpp"
        "assets/launcher_animation_frames.cpp"

        "media/library/media_library.cpp"
        "media/library/media_index_store.cpp"
        "media/library/media_catalog_v2.cpp"
        "media/library/media_groups_v2.cpp"
        "media/library/media_catalog_store_v2.cpp"
        "media/library/media_probe.cpp"
        "media/library/media_metadata.cpp"
        "media/library/media_artwork.cpp"

        "player/player_playlist.cpp"
        "player/player_state.cpp"
        "player/player_transport.cpp"
        "player/player_control.cpp"

        "ui/ui_manager.cpp"
        "ui/ui_common.cpp"
        "ui/input/touch_input.cpp"
        "ui/gesture/gesture_router.cpp"
        "ui/font/font_manager.cpp"
        "ui/search/pinyin_initial.cpp"
        "ui/search/search_key_builder.cpp"
        "ui/lyrics/lyrics_service.cpp"
        "ui/lyrics/lyrics_view.cpp"
        "ui/spectrum/spectrum_view.cpp"
        "ui/widgets/quick_index_keyboard.cpp"
        "ui/artwork/cover_surface_cache.cpp"
        "ui/artwork/now_playing_artwork.cpp"
        "ui/screens/player_home.cpp"
        "ui/screens/library_view.cpp"

    INCLUDE_DIRS
        "."

        "board"

        "boot"

        "app"

        "system"

        "drivers/i2c"

        "drivers/touch"

        "drivers/imu"

        "drivers/storage"

        "drivers/display"

        "drivers/audio"

        "audio"
        "audio/decoders"
        "audio/sources"

        "assets"

        "media/library"

        "player"

        "ui"
        "ui/artwork"
        "ui/input"
        "ui/lyrics"
        "ui/spectrum"

    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_sdmmc
        esp_driver_spi
        esp_lcd
        esp_lv_decoder
        esp_new_jpeg
        libpng
        esp_psram
        esp_timer
        nvs_flash
        fatfs
)

# SRAM.1.3：LVGL 仍使用内置 TLSF，但把固定 pool 扩容到 128KB，并继续全部放在 PSRAM。
# 64KB 在 Music PreserveBackground + Ebook 39 行目录 + 隐藏 Reader 控件同时存在时会逼近上限，
# 第一帧刷新还需要临时 draw task，lv_malloc_zeroed() 失败会在 LVGL 9.2.2 draw 链里触发 StoreProhibited。
# 这里显式定义 LV_MEM_SIZE=128KB，同时保留 PSRAM backing；不增加 INTERNAL SRAM 占用。
# 显示 DMA buffer 仍由 esp_lvgl_port 按 INTERNAL/DMA 能力分配，不受本配置影响。
if(NOT TARGET __idf_lvgl__lvgl)
    message(FATAL_ERROR "FakePod SRAM.1.3: LVGL component target 不可用，拒绝静默退回 INTERNAL pool")
endif()

if(NOT TARGET idf::heap)
    message(FATAL_ERROR "FakePod SRAM.1.3: ESP-IDF heap component target 不可用")
endif()

target_include_directories(
    __idf_lvgl__lvgl
    PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/ui/lvgl_config"
)

target_compile_definitions(
    __idf_lvgl__lvgl
    PRIVATE
        LV_MEM_SIZE=131072U
        LV_MEM_POOL_INCLUDE=\"fakepod_lvgl_pool.h\"
        LV_MEM_POOL_ALLOC=fakepod_lvgl_psram_pool_alloc
)

# 128KB LVGL pool 由 fakepod_lvgl_pool.h 使用 heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) 分配。
target_link_libraries(__idf_lvgl__lvgl PRIVATE idf::heap)

message(STATUS "FakePod: LVGL 128KB builtin TLSF pool 强制使用 PSRAM backing store")

# ESP-IDF 的 RGB LCD 驱动在 Windows + Xtensa GCC 14.2.0 上可能在 IRA 阶段触发编译器内部错误。
# FakePod 实际使用 QSPI CO5300，不走 RGB LCD 数据路径，但 esp_lvgl_port 链接时仍需要该驱动导出的符号。
# 因此保留该源文件参与编译，仅对它单独使用 -O0，避免影响项目其他文件的优化和并行编译。
if(TARGET __idf_esp_lcd)
    set(_fakepod_rgb_lcd_source "$ENV{IDF_PATH}/components/esp_lcd/rgb/esp_lcd_panel_rgb.c")
    if(EXISTS "${_fakepod_rgb_lcd_source}")
        set_source_files_properties(
            "${_fakepod_rgb_lcd_source}"
            TARGET_DIRECTORY __idf_esp_lcd
            PROPERTIES COMPILE_OPTIONS "-O0"
        )
        message(STATUS "FakePod: esp_lcd_panel_rgb.c 单文件使用 -O0，规避 Xtensa GCC IRA ICE")
    endif()
endif()
