idf_component_register(
    SRCS
        "main.cpp"

        "boot/boot_state.cpp"

        "system/system_loop.cpp"
        "system/system_runtime.cpp"

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
        "audio/audio_spectrum_snapshot.cpp"
        "audio/sources/audio_source.cpp"
        "audio/sources/sd_file_audio_source.cpp"
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
        fatfs
)

# SRAM.1：LVGL 仍使用内置 TLSF，但其 64KB memory pool 改由 PSRAM 提供。
# 只修改 LVGL component 的配置可见性；显示 DMA buffer 仍由 esp_lvgl_port 按 INTERNAL/DMA 能力分配。
if(TARGET __idf_lvgl__lvgl)
    target_include_directories(
        __idf_lvgl__lvgl
        PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/ui/lvgl_config"
    )
    target_compile_definitions(__idf_lvgl__lvgl PRIVATE LV_CONF_INCLUDE_SIMPLE=1)

    # lv_conf.h 使用 heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)。
    # 显式链接 heap component，保证 LVGL component 编译时可见 esp_heap_caps.h。
    if(TARGET idf::heap)
        target_link_libraries(__idf_lvgl__lvgl PRIVATE idf::heap)
    endif()

    message(STATUS "FakePod: LVGL 64KB builtin pool 使用 PSRAM backing store")
endif()

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
