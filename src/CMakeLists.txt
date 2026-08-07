idf_component_register(
    SRCS
        "main.cpp"

        "boot/boot_state.cpp"

        "system/system_loop.cpp"

        "drivers/i2c/i2c_bus.cpp"

        "drivers/touch/cst820.cpp"

        "drivers/imu/qmi8658.cpp"

        "drivers/storage/sdcard.cpp"

    INCLUDE_DIRS
        "."

        "board"

        "boot"

        "system"

        "drivers/i2c"

        "drivers/touch"

        "drivers/imu"

        "drivers/storage"

    REQUIRES
        esp_driver_i2c
        esp_driver_sdmmc
        esp_psram
        fatfs
)