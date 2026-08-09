#include "quick_index_keyboard.h"

#include "font/font_manager.h"
#include "ui_common.h"

static constexpr int32_t QUICK_INDEX_COLUMNS = 5;
static constexpr int32_t QUICK_INDEX_ROWS = 2;
static constexpr int32_t QUICK_INDEX_GAP_X = 6;
static constexpr int32_t QUICK_INDEX_GAP_Y = 8;
static constexpr int32_t QUICK_INDEX_KEY_RADIUS = 11;

static void quick_index_keyboard_apply_key_style(QuickIndexKeyboard *keyboard, uint8_t key_index)
{
    if (keyboard == nullptr || key_index >= QUICK_INDEX_KEY_COUNT || keyboard->buttons[key_index] == nullptr) {
        return;
    }

    const bool selected = keyboard->selected_key == static_cast<int8_t>(key_index);
    lv_obj_set_style_bg_color(
        keyboard->buttons[key_index],
        lv_color_hex(selected ? 0x2F6F9F : 0x1D2A38),
        LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(
        keyboard->buttons[key_index],
        lv_color_hex(selected ? 0x3C82B8 : 0x273A4F),
        LV_STATE_PRESSED);
    lv_obj_set_style_border_color(
        keyboard->buttons[key_index],
        lv_color_hex(selected ? 0x74B5E3 : 0x31465B),
        LV_STATE_DEFAULT);
    if (keyboard->labels[key_index] != nullptr) {
        lv_obj_set_style_text_color(
            keyboard->labels[key_index],
            lv_color_hex(selected ? 0xFFFFFF : 0xDCE8F3),
            0);
    }
}

static void quick_index_keyboard_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    auto *binding = static_cast<QuickIndexKeyBinding *>(lv_event_get_user_data(event));
    if (binding == nullptr || binding->owner == nullptr || binding->key_index >= QUICK_INDEX_KEY_COUNT) {
        return;
    }

    QuickIndexKeyboard *keyboard = binding->owner;
    if (keyboard->callback != nullptr) {
        keyboard->callback(binding->key_index, keyboard->user_data);
    }
}

bool quick_index_keyboard_create(
    QuickIndexKeyboard *keyboard,
    lv_obj_t *parent,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    QuickIndexKeyboardCallback callback,
    void *user_data)
{
    if (keyboard == nullptr || parent == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    *keyboard = {};
    keyboard->x = x;
    keyboard->y = y;
    keyboard->width = width;
    keyboard->height = height;
    keyboard->callback = callback;
    keyboard->user_data = user_data;

    keyboard->root = lv_obj_create(parent);
    if (keyboard->root == nullptr) {
        return false;
    }
    ui_common_lock_object(keyboard->root);
    lv_obj_set_pos(keyboard->root, x, y);
    lv_obj_set_size(keyboard->root, width, height);
    lv_obj_set_style_radius(keyboard->root, 0, 0);
    lv_obj_set_style_bg_opa(keyboard->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keyboard->root, 0, 0);
    lv_obj_set_style_pad_all(keyboard->root, 0, 0);
    lv_obj_clear_flag(keyboard->root, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t key_width = (width - QUICK_INDEX_GAP_X * (QUICK_INDEX_COLUMNS - 1)) / QUICK_INDEX_COLUMNS;
    const int32_t key_height = (height - QUICK_INDEX_GAP_Y * (QUICK_INDEX_ROWS - 1)) / QUICK_INDEX_ROWS;

    for (uint8_t index = 0U; index < QUICK_INDEX_KEY_COUNT; ++index) {
        const int32_t row = index / QUICK_INDEX_COLUMNS;
        const int32_t column = index % QUICK_INDEX_COLUMNS;
        const int32_t key_x = column * (key_width + QUICK_INDEX_GAP_X);
        const int32_t key_y = row * (key_height + QUICK_INDEX_GAP_Y);

        keyboard->buttons[index] = lv_button_create(keyboard->root);
        if (keyboard->buttons[index] == nullptr) {
            return false;
        }
        ui_common_lock_object(keyboard->buttons[index]);
        lv_obj_set_pos(keyboard->buttons[index], key_x, key_y);
        lv_obj_set_size(keyboard->buttons[index], key_width, key_height);
        lv_obj_set_style_radius(keyboard->buttons[index], QUICK_INDEX_KEY_RADIUS, 0);
        lv_obj_set_style_bg_opa(keyboard->buttons[index], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(keyboard->buttons[index], 1, 0);
        lv_obj_set_style_border_color(keyboard->buttons[index], lv_color_hex(0x31465B), 0);
        lv_obj_set_style_border_opa(keyboard->buttons[index], LV_OPA_70, 0);
        lv_obj_set_style_shadow_width(keyboard->buttons[index], 0, 0);
        lv_obj_set_style_pad_all(keyboard->buttons[index], 0, 0);

        keyboard->labels[index] = lv_label_create(keyboard->buttons[index]);
        if (keyboard->labels[index] == nullptr) {
            return false;
        }
        ui_common_lock_object(keyboard->labels[index]);
        lv_label_set_text(keyboard->labels[index], "");
        lv_obj_set_style_text_font(keyboard->labels[index], font_manager_get_ui_font(), 0);
        lv_obj_set_style_text_color(keyboard->labels[index], lv_color_hex(0xE9ECF1), 0);
        lv_obj_center(keyboard->labels[index]);

        keyboard->bindings[index].owner = keyboard;
        keyboard->bindings[index].key_index = index;
        lv_obj_add_event_cb(
            keyboard->buttons[index],
            quick_index_keyboard_clicked_cb,
            LV_EVENT_CLICKED,
            &keyboard->bindings[index]);
        quick_index_keyboard_apply_key_style(keyboard, index);
    }

    return true;
}

void quick_index_keyboard_set_labels(
    QuickIndexKeyboard *keyboard,
    const char *const labels[QUICK_INDEX_KEY_COUNT])
{
    if (keyboard == nullptr || labels == nullptr) {
        return;
    }

    for (uint8_t index = 0U; index < QUICK_INDEX_KEY_COUNT; ++index) {
        if (keyboard->labels[index] != nullptr) {
            lv_label_set_text(keyboard->labels[index], labels[index] != nullptr ? labels[index] : "");
        }
    }
}

void quick_index_keyboard_set_selected(QuickIndexKeyboard *keyboard, int8_t key_index)
{
    if (keyboard == nullptr) {
        return;
    }
    if (key_index < -1 || key_index >= static_cast<int8_t>(QUICK_INDEX_KEY_COUNT)) {
        key_index = -1;
    }
    keyboard->selected_key = key_index;
    for (uint8_t index = 0U; index < QUICK_INDEX_KEY_COUNT; ++index) {
        quick_index_keyboard_apply_key_style(keyboard, index);
    }
}

void quick_index_keyboard_set_visible(QuickIndexKeyboard *keyboard, bool visible)
{
    if (keyboard == nullptr || keyboard->root == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(keyboard->root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(keyboard->root, LV_OBJ_FLAG_HIDDEN);
    }
}

bool quick_index_keyboard_contains_point(const QuickIndexKeyboard *keyboard, int16_t x, int16_t y)
{
    if (keyboard == nullptr || keyboard->root == nullptr || lv_obj_has_flag(keyboard->root, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    return x >= keyboard->x && x < keyboard->x + keyboard->width &&
        y >= keyboard->y && y < keyboard->y + keyboard->height;
}
