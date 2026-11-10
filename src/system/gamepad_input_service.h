#pragma once

#include <stddef.h>
#include <stdint.h>

// Raw BLE HID input surface for future emulator/input adapters.
// This layer deliberately does not assume a particular gamepad report layout.
static constexpr size_t GAMEPAD_INPUT_REPORT_MAX_BYTES = 64U;
static constexpr size_t GAMEPAD_INPUT_REPORT_MAP_MAX_BYTES = 512U;
static constexpr size_t GAMEPAD_INPUT_DEVICE_NAME_MAX = 48U;

struct GamepadInputSnapshot {
    bool connected = false;
    bool battery_valid = false;
    uint8_t battery_percent = 0U;
    uint8_t usage = 0U; // esp_hid_usage_t numeric value; kept IDF-independent here.
    uint16_t vendor_id = 0U;
    uint16_t product_id = 0U;
    uint16_t report_id = 0U;
    uint8_t map_index = 0U;
    uint8_t report_length = 0U;
    uint8_t report[GAMEPAD_INPUT_REPORT_MAX_BYTES] = {};
    uint32_t report_sequence = 0U;
    char device_name[GAMEPAD_INPUT_DEVICE_NAME_MAX] = {};
};

void gamepad_input_service_reset();
void gamepad_input_service_set_connected(
    const char *name, uint16_t vendor_id, uint16_t product_id, uint8_t usage,
    const uint8_t *report_map, size_t report_map_length);
void gamepad_input_service_set_disconnected();
void gamepad_input_service_set_battery(uint8_t percent);
void gamepad_input_service_submit_report(
    uint8_t usage, uint16_t report_id, uint8_t map_index,
    const uint8_t *data, size_t length);
bool gamepad_input_service_get_snapshot(GamepadInputSnapshot *out_snapshot);
// Returns the full report-map length; copies up to capacity bytes when dst is non-null.
size_t gamepad_input_service_copy_report_map(uint8_t *dst, size_t capacity);
