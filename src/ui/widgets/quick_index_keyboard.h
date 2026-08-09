#pragma once

#include <stdint.h>

#include "lvgl.h"

static constexpr uint8_t QUICK_INDEX_KEY_COUNT = 10U;

struct QuickIndexKeyboard;
using QuickIndexKeyboardCallback = void (*)(uint8_t key_index, void *user_data);

struct QuickIndexKeyBinding
{
    QuickIndexKeyboard *owner = nullptr;
    uint8_t key_index = 0;
};

struct QuickIndexKeyboard
{
    lv_obj_t *root = nullptr;
    lv_obj_t *buttons[QUICK_INDEX_KEY_COUNT] = {};
    lv_obj_t *labels[QUICK_INDEX_KEY_COUNT] = {};
    QuickIndexKeyBinding bindings[QUICK_INDEX_KEY_COUNT] = {};
    QuickIndexKeyboardCallback callback = nullptr;
    void *user_data = nullptr;
    int8_t selected_key = -1;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

// 2x5 快速索引键盘。所有文字使用 FakePod UI 主字体，不使用 LVGL 西文字体。
bool quick_index_keyboard_create(
    QuickIndexKeyboard *keyboard,
    lv_obj_t *parent,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    QuickIndexKeyboardCallback callback,
    void *user_data);

void quick_index_keyboard_set_labels(
    QuickIndexKeyboard *keyboard,
    const char *const labels[QUICK_INDEX_KEY_COUNT]);

void quick_index_keyboard_set_selected(QuickIndexKeyboard *keyboard, int8_t key_index);
void quick_index_keyboard_set_visible(QuickIndexKeyboard *keyboard, bool visible);
bool quick_index_keyboard_contains_point(const QuickIndexKeyboard *keyboard, int16_t x, int16_t y);
