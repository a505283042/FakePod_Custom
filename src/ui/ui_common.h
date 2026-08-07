#pragma once

#include "lvgl.h"

// 禁止对象参与滚动、回弹、惯性和滚动链。
void ui_common_lock_object(lv_obj_t *obj);
