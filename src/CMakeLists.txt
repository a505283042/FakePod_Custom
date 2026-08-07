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

        "media/library/media_library.cpp"

        "player/player_state.cpp"

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

        "media/library"

        "player"

        "ui"

    REQUIRES
        esp_driver_i2c
        esp_driver_sdmmc
        esp_driver_spi
        esp_lcd
        esp_psram
        esp_timer
        fatfs
)