#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

// Display/LVGL 适配层内部接口。业务模块不应直接获取 Panel IO / Panel handle。
esp_lcd_panel_io_handle_t display_get_panel_io();
esp_lcd_panel_handle_t display_get_panel();

// esp_lvgl_port_add_disp() 会覆盖 display_init() 阶段的 color-done callback；
// UI manager 创建 LVGL display 后调用此接口恢复统一 flush_ready 桥接。
esp_err_t display_install_lvgl_color_done_bridge(void *lvgl_display);

// CO5300 TE 同步只供 LVGL 刷新和 BoundedSPI transport 使用。
bool display_te_is_ready();
bool display_te_wait_next(uint32_t timeout_ms);
uint32_t display_te_get_period_us();
