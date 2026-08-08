idf_component_register(
    SRCS
        "main.cpp"

        "boot/boot_state.cpp"

        "system/system_loop.cpp"

        "drivers/i2c/i2c_bus.cpp"

        "drivers/touch/cst820.cpp"

        "drivers/imu/qmi8658.cpp"

        "drivers/storage/sdcard.cpp"

        "drivers/display/display.cpp"

        "drivers/audio/cs43131.cpp"
        "drivers/audio/i2s_output.c"

        "audio/audio_service.cpp"
        "audio/decoders/pcm_decoder.cpp"
        "audio/decoders/wav_decoder.cpp"
        "audio/decoders/flac_decoder.cpp"
        "audio/decoders/mp3_decoder.cpp"

        "media/library/media_library.cpp"

        "player/player_state.cpp"
        "player/player_control.cpp"

        "ui/ui_manager.cpp"
        "ui/ui_common.cpp"
        "ui/font/font_manager.cpp"
        "ui/screens/player_home.cpp"

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

        "media/library"

        "player"

        "ui"

    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_sdmmc
        esp_driver_spi
        esp_lcd
        esp_psram
        esp_timer
        fatfs
)

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
