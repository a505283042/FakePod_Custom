#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// ============================================================
// 屏幕电源状态（AMOLED 三态）
// ============================================================
typedef enum ScreenPowerState : uint8_t
{
    ScreenPowerNormal = 0,    // 正常显示：亮度 60
    ScreenPowerAOD    = 1,    // AMOLED 息屏显示：中央 [锁]+灰字，亮度 15，30s ±1px 抖动防烧屏
    ScreenPowerOff    = 2     // 真·熄屏：brightness=0，暂停显示输出
} ScreenPowerState;

// ============================================================
// 锁定状态
// 锁定时 ui_touch_read_cb 拦截所有触摸输入（返回 RELEASED）。
// ============================================================
typedef enum ScreenLockState : uint8_t
{
    ScreenLockUnlocked = 0,
    ScreenLockLocked   = 1
} ScreenLockState;

// ============================================================
// GPIO0 长按 → 屏幕动作菜单
// 菜单打开后常驻屏幕（GPIO0 松手也不消失），点击某一行才执行。
// 再按一次 GPIO0（任何档位）= 关闭菜单（取消不执行）。
// ============================================================
typedef enum ScreenActionRow : uint8_t
{
    ScreenActionRowToggleLock = 0,   // 🔒 锁屏
    ScreenActionRowAOD         = 1,  // AOD 息屏显示（[锁]+灰字，无时钟）
    ScreenActionRowScreenOff   = 2,  // 熄屏（全黑）
    ScreenActionRowCancel      = 3,  // 取消 / 返回
    ScreenActionRowCount       = 4
} ScreenActionRow;

// ============================================================
// 初始化 / 释放
// ============================================================
esp_err_t screen_lock_simple_create(void);
void      screen_lock_simple_destroy(void);

// ============================================================
// 状态查询
// ============================================================
ScreenPowerState screen_lock_simple_get_power(void);   // 新命名，避免与 set 混用
ScreenLockState  screen_lock_simple_get_lock(void);
bool             screen_lock_is_locked(void);          // 老函数名继续保留（兼容）
ScreenPowerState screen_lock_get_power(void);          // 老别名，返回同一个
ScreenLockState  screen_lock_get_lock(void);           // 老别名

// ============================================================
// 状态切换
// ============================================================
void screen_lock_simple_set_power(ScreenPowerState power);   // 推荐新命名
void screen_lock_simple_set_lock(ScreenLockState lock);
void screen_lock_set_power(ScreenPowerState power);          // 老别名
void screen_lock_set_lock(ScreenLockState lock);             // 老别名
void screen_lock_toggle_lock(void);
void screen_lock_wake_if_needed(void);

// ============================================================
// 触摸拦截钩子（ui_manager.cpp 在 ui_touch_read_cb 最开头调用）
// ============================================================
bool screen_lock_should_block_touch(void);

// ============================================================
// 动作菜单
// ============================================================

// 打开菜单（长按 GPIO0 达到档位时调用）。屏幕在暗态时已由 gpio0_service 提前解锁+亮屏，
// 所以这里进来一定是 Normal，直接显示即可。
void screen_action_menu_open(void);

// 关闭菜单。
// execute_if_valid=true：若当前高亮行不是 Cancel → 先执行该行再关闭
//                   =false：直接关闭（取消，不执行）
void screen_action_menu_close(bool execute_if_valid);

// 关闭菜单，不执行（用于老代码路径兼容：screen_action_menu_close() 无参数 = false）
inline void screen_action_menu_close(void) { screen_action_menu_close(false); }

// 查询菜单是否打开
bool screen_action_menu_is_open(void);

// （新）由 ui_manager 的触摸钩子直接调用，避开 LVGL 对象 CLICKABLE 事件链的不确定性。
//   press(x,y) ：DOWN 边沿，坐标在卡片行内则设置高亮 + 记录"这一行按下了"
//   release(x,y)：UP 边沿，如果记录的按下行就是当前命中行 → 执行该行并关闭菜单
//   两者都返回 true 表示"本次触摸已由菜单消费（LVGL 不再分发给下层）"
bool screen_action_menu_on_touch_press(int x, int y);
bool screen_action_menu_on_touch_release(int x, int y);

// （旧签名兼容）不再推荐使用，内部转 press
bool screen_action_menu_handle_touch_down(int x, int y);
void screen_action_menu_handle_touch_up(void);

// （兼容老签名，内部转 UP 时 execute）
bool screen_action_menu_handle_touch(int x, int y);
void screen_action_menu_handle_drag(int y);
void screen_action_menu_set_highlight(ScreenActionRow row);
void screen_action_menu_confirm_and_close(void);

// ============================================================
// 渲染 / 心跳（system_loop 中调用即可）
// AOD 内部： 30s ±1px 抖动（不再有「时钟刷新」——硬件无 RTC）
// ============================================================
void screen_lock_simple_render(void);

// ============================================================
// 层级置顶（其它页面 move_foreground 之后调用）
// ============================================================
void screen_lock_simple_raise(void);
