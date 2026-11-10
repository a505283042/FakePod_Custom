#include "gamepad_input_service.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

namespace {
static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static GamepadInputSnapshot g_snapshot = {};
static uint8_t g_report_map[GAMEPAD_INPUT_REPORT_MAP_MAX_BYTES] = {};
static size_t g_report_map_length = 0U;
}

void gamepad_input_service_reset()
{
    portENTER_CRITICAL(&g_lock);
    g_snapshot = {};
    memset(g_report_map, 0, sizeof(g_report_map));
    g_report_map_length = 0U;
    portEXIT_CRITICAL(&g_lock);
}

void gamepad_input_service_set_connected(
    const char *name, uint16_t vendor_id, uint16_t product_id, uint8_t usage,
    const uint8_t *report_map, size_t report_map_length)
{
    portENTER_CRITICAL(&g_lock);
    g_snapshot.connected = true;
    g_snapshot.vendor_id = vendor_id;
    g_snapshot.product_id = product_id;
    g_snapshot.usage = usage;
    g_snapshot.report_length = 0U;
    g_snapshot.report_id = 0U;
    g_snapshot.map_index = 0U;
    g_snapshot.report_sequence = 0U;
    if (name != nullptr) {
        snprintf(g_snapshot.device_name, sizeof(g_snapshot.device_name), "%s", name);
    } else {
        snprintf(g_snapshot.device_name, sizeof(g_snapshot.device_name), "%s", "BLE HID Gamepad");
    }
    memset(g_report_map, 0, sizeof(g_report_map));
    g_report_map_length = report_map != nullptr
        ? (report_map_length < sizeof(g_report_map) ? report_map_length : sizeof(g_report_map))
        : 0U;
    if (g_report_map_length > 0U) {
        memcpy(g_report_map, report_map, g_report_map_length);
    }
    portEXIT_CRITICAL(&g_lock);
}

void gamepad_input_service_set_disconnected()
{
    portENTER_CRITICAL(&g_lock);
    g_snapshot.connected = false;
    g_snapshot.battery_valid = false;
    g_snapshot.battery_percent = 0U;
    g_snapshot.report_length = 0U;
    g_snapshot.report_id = 0U;
    g_snapshot.map_index = 0U;
    g_snapshot.report_sequence = 0U;
    g_snapshot.device_name[0] = '\0';
    g_snapshot.vendor_id = 0U;
    g_snapshot.product_id = 0U;
    g_snapshot.usage = 0U;
    memset(g_snapshot.report, 0, sizeof(g_snapshot.report));
    memset(g_report_map, 0, sizeof(g_report_map));
    g_report_map_length = 0U;
    portEXIT_CRITICAL(&g_lock);
}

void gamepad_input_service_set_battery(uint8_t percent)
{
    if (percent > 100U) percent = 100U;
    portENTER_CRITICAL(&g_lock);
    g_snapshot.battery_percent = percent;
    g_snapshot.battery_valid = true;
    portEXIT_CRITICAL(&g_lock);
}

void gamepad_input_service_submit_report(
    uint8_t usage, uint16_t report_id, uint8_t map_index,
    const uint8_t *data, size_t length)
{
    if (data == nullptr || length == 0U) return;
    if (length > GAMEPAD_INPUT_REPORT_MAX_BYTES) length = GAMEPAD_INPUT_REPORT_MAX_BYTES;
    portENTER_CRITICAL(&g_lock);
    if (g_snapshot.connected) {
        g_snapshot.usage = usage;
        g_snapshot.report_id = report_id;
        g_snapshot.map_index = map_index;
        g_snapshot.report_length = static_cast<uint8_t>(length);
        memcpy(g_snapshot.report, data, length);
        if (length < sizeof(g_snapshot.report)) {
            memset(g_snapshot.report + length, 0, sizeof(g_snapshot.report) - length);
        }
        ++g_snapshot.report_sequence;
    }
    portEXIT_CRITICAL(&g_lock);
}

bool gamepad_input_service_get_snapshot(GamepadInputSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    portENTER_CRITICAL(&g_lock);
    *out_snapshot = g_snapshot;
    portEXIT_CRITICAL(&g_lock);
    return true;
}

size_t gamepad_input_service_copy_report_map(uint8_t *dst, size_t capacity)
{
    portENTER_CRITICAL(&g_lock);
    const size_t full_length = g_report_map_length;
    if (dst != nullptr && capacity > 0U) {
        const size_t copy_length = full_length < capacity ? full_length : capacity;
        if (copy_length > 0U) memcpy(dst, g_report_map, copy_length);
    }
    portEXIT_CRITICAL(&g_lock);
    return full_length;
}
