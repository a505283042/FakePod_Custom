#include "screen_lock_simple.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "audio_service.h"          // AudioStateSnapshot（进度/曲目索引）
#include "board_pins.h"
#include "display.h"
#include "font/font_manager.h"
#include "lyrics/lyrics_service.h"  // LyricsWindowSnapshot（当前一句歌词）
#include "media/library/media_catalog_v2.h"  // 歌曲标题/歌手 (catalog)
#include "media/library/media_library.h"     // 歌曲 fallback 文件名 / technical info (含 duration_ms)
#include "player_state.h"           // player_state_get_index / get_format / ...
// 页面切换专用：AOD 不再是"遮罩"，而是一个真正的独立页面 ——
//   进入 AOD 时把 4 个音乐页整个后台/关闭，切歌时 player_home 已后台化，
//   不会再触发封面 move_foreground → 彻底杜绝"封面从 AOD 底下露出来"。
#include "ui/lyrics/lyrics_view.h"
#include "ui/spectrum/spectrum_view.h"
#include "ui/screens/library_view.h"
#include "ui/screens/player_home.h"

// 外部 brightness/disp_on_off 入口（CO5300 AMOLED）
extern "C" esp_err_t esp_lcd_panel_co5300_set_brightness(esp_lcd_panel_handle_t handle, uint8_t level);
extern esp_lcd_panel_handle_t display_get_panel_handle(void);

static const char *TAG = "屏幕锁";

// ============================================================
// 常量
// ============================================================
static constexpr uint8_t  kBrightnessNormal = 60U;    // 同 display_reveal_after_first_frame
static constexpr uint8_t  kBrightnessAOD    = 15U;    // AMOLED AOD 极低亮度
static constexpr uint8_t  kBrightnessOff    = 0U;
static constexpr uint32_t kJitterIntervalMs = 30000U; // 30s 防烧屏抖动
static constexpr int8_t   kJitterMaxPx      = 1;     // ±1px

// 颜色常量（菜单胶囊用曲库「专辑」主题的紫色调，和 library_view Albums 同系）
//   同源色板：library_view.cpp Albums 主题
//     列表页 header=0xB5A0DD  accent=0xA28ACC  row_current_bg=0x332744
//     详情页 header=0x8E79B1  accent=0x806DA5  row_current_bg=0x2B2138
static constexpr uint32_t kC_BgScreen         = 0x000000;  // 背景纯黑
static constexpr uint32_t kC_CardPanel        = 0x05070B;  // 同 Launcher 中心圆
static constexpr uint32_t kC_CardPanelBorder  = 0x161B27;  // 同 Launcher 中心圆边框
static constexpr uint32_t kC_RowUnselBg       = 0x332744;  // 专辑主题深紫底（row_current_bg）
static constexpr uint8_t  kC_RowUnselBgOpa    = 225U;      // ≈88%：非常实的紫底
static constexpr uint32_t kC_RowSelBg         = 0x806DA5;  // 选中：专辑详情页 accent 紫
static constexpr uint8_t  kC_RowSelBgOpa      = 240U;      // ≈94%：一眼能辨识选中状态
static constexpr uint32_t kC_RowUnselText     = 0xB5A0DD;  // 专辑列表页 header 紫文字
static constexpr uint32_t kC_RowSelText       = 0xFFFFFF;  // 选中：纯白加粗字
static constexpr uint32_t kC_TipText          = 0x8890A3;  // 底部提示

// ============================================================
// 运行状态
// ============================================================
static bool              g_ready         = false;
static ScreenPowerState  g_power         = ScreenPowerNormal;
static ScreenLockState   g_lock          = ScreenLockUnlocked;
static TickType_t        g_last_jitter_tick = 0;
static int8_t            g_jitter_x      = 0;
static int8_t            g_jitter_y      = 0;

// AOD 作为"独立页面"前后台切换时的恢复线索（AOD 进入时保存，退出时恢复）：
//   true 表示进入 AOD 之前歌词/频谱/曲库 有任意一个前台可见，
//   退出 AOD 时不走 player_home_resume_from_fullscreen_view（避免把前台页面弹回首页）。
static bool g_aod_prev_was_fullscreen = false;
static bool g_aod_prev_library_visible = false;
static bool g_aod_prev_music_foreground = true;

// LVGL 对象（AOD / 锁 悬浮层）
static lv_obj_t *g_aod_root        = nullptr;   // AOD 全屏容器（屏态 = AOD 才显示）

// AOD 音乐信息卡（由上至下 4 行，AMOLED 低灰字 + 防烧屏抖动）：
//   ① 歌曲标题 + 歌手（"Title - Artist"，36×372，单行，中灰）
//   ② 当前一句歌词（36×360，最多 2 行，亮灰，主视觉）
//   ③ 进度条（薄条 4×360，深灰底+中灰已播放）
//   ④ 时间文字（"当前 / 总时长"，30×372，中灰）
static lv_obj_t *g_aod_title      = nullptr;
static lv_obj_t *g_aod_lyric      = nullptr;
static lv_obj_t *g_aod_progress   = nullptr;   // lv_bar
static lv_obj_t *g_aod_time       = nullptr;

// AOD 右上角小锁：只做提醒（暗灰小字尺寸，不抢视觉）。
static lv_obj_t *g_aod_tip_lock   = nullptr;

// AOD 刷新跟踪（只在数据变化时 set_text，避免每 5ms 重画烧屏）
static uint32_t g_aod_last_track_idx       = UINT32_MAX;
static uint32_t g_aod_last_lyric_line_idx  = UINT32_MAX;
static uint64_t g_aod_last_pos_ms          = UINT64_MAX;
static uint64_t g_aod_last_total_ms        = UINT64_MAX;
static uint32_t g_aod_last_refresh_tick    = 0U;
static constexpr uint32_t kAODRefreshTickMs = 200U;   // 歌词/时间 200ms 刷新一次（解决歌词偏迟，
                                                        //   AudioStateSnapshot.position_ms 本身 250ms
                                                        //   更新，所以 200ms 基本同步播放进度）

static lv_obj_t *g_lock_icon       = nullptr;   // Normal 页面右上角 [锁]（Locked 显示）

// ========== 动作菜单 ==========
static bool       g_menu_open       = false;
static ScreenActionRow g_menu_highlight = ScreenActionRowToggleLock;
static lv_obj_t  *g_menu_root       = nullptr;
static lv_obj_t  *g_menu_panel      = nullptr;
static lv_obj_t  *g_menu_rows[static_cast<int>(ScreenActionRowCount)] = {};
static lv_obj_t  *g_menu_tip        = nullptr;
// 触摸命中矩形按下跟踪（触摸钩子直接用，不依赖 LVGL 对象 CLICKABLE）
static int        g_press_row       = -1;   // -1=卡片外按下，>=0=按下的行索引，UP 时同值则执行
static int        g_press_x         = 0;
static int        g_press_y         = 0;

static constexpr int  kMenuPanelWidth   = 380;
static constexpr int  kMenuRowHeight    = 80;
static constexpr int  kMenuRowGap       = 14;       // 与底部坐标计算的 kMENU_ROW_GAP 保持一致（较宽松）
static constexpr int  kMenuPanelRadius  = 24;
static constexpr int  kMenuRowRadius    = LV_RADIUS_CIRCLE;

// 菜单行绝对坐标常量（FAKEPOD_LCD=460×460，行宽 380、行高 80、间距 14）
//   └─ 4 行总高 = 4×80 + 3×14 = 362 → 垂直居中 y=(460-362)/2=49
//   └─ 水平居中 x=(460-380)/2=40
//  供 menu_create_ui（视觉位置） 和 row_screen_rect_y（触摸命中矩形） 共用一个来源，
//  避免两处计算不同导致视觉/触摸偏移。
static constexpr int kMENU_ROW_WIDTH = kMenuPanelWidth;                                    // 380
static constexpr int kMENU_ROW_GAP   = kMenuRowGap;                                        // 14
static constexpr int kMENU_ROW_XLEFT = (FAKEPOD_LCD_WIDTH - kMENU_ROW_WIDTH) / 2;          // 40
static constexpr int kMENU_BLOCK_TOP =
    (FAKEPOD_LCD_HEIGHT - (kMenuRowHeight * static_cast<int>(ScreenActionRowCount)
                          + kMENU_ROW_GAP * (static_cast<int>(ScreenActionRowCount) - 1))) / 2;  // 49

static const char *kRowText[static_cast<int>(ScreenActionRowCount)] = {
    "锁屏",                     // Row 0：正常使用最多
    "AOD 息屏显示",              // Row 1
    "熄屏（全黑）",              // Row 2
    "取消 / 返回"                // Row 3
};

static void aod_create_ui();    // 前置声明
static void menu_create_ui();   // 前置声明

// ============================================================
// 本地工具函数
// ============================================================
static void display_panel_apply_brightness(uint8_t level)
{
    extern esp_lcd_panel_handle_t display_get_panel_handle(void);
    esp_lcd_panel_handle_t panel = display_get_panel_handle();
    if (panel == nullptr) {
        ESP_LOGW(TAG, "display 未就绪，跳过亮度写入 level=%u", level);
        return;
    }
    const esp_err_t ret = esp_lcd_panel_co5300_set_brightness(panel, level);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "设置 AMOLED 亮度失败 level=%u %s", level, esp_err_to_name(ret));
    }
}

static void display_panel_apply_output(bool on)
{
    extern esp_lcd_panel_handle_t display_get_panel_handle(void);
    esp_lcd_panel_handle_t panel = display_get_panel_handle();
    if (panel == nullptr) {
        ESP_LOGW(TAG, "display 未就绪，跳过显示开/关 on=%d", on);
        return;
    }
    const esp_err_t ret = esp_lcd_panel_disp_on_off(panel, on);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "切换显示输出 on=%d 失败 %s", on, esp_err_to_name(ret));
    }
}

static void obj_set_visible(lv_obj_t *obj, bool vis)
{
    if (obj == nullptr) return;
    if (vis) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else     lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================
// 锁图标：「方法 A — 子对象堆叠」（和曲库 🔍 放大镜完全同范式：
//         library_view.cpp:381-411 的画法——多个 lv_obj_create 叠加，
//         不写 DRAW_MAIN 回调，不依赖 lv_draw_rect / lv_layer API）。
//
// 经典锁结构（use × use 的正方形使用区域内，pad 边距）：
//     ┃          ┃   post：锁梁左右竖线（beam_w 粗，post_h 高）
//     ┗━━━━━━━━━━┛   bar ：顶横条（beam_w 粗，spanning 全宽）
//     ┌──────────┐
//     │    ●     │   body：锁身矩形（body_h，body_r 圆角）+ 锁芯圆
//     └──────────┘
// 返回值：最外层容器（大小 W×H），后续通过 lv_obj_set_pos / lv_obj_center 摆放。
// ============================================================
struct LockWidgetSpec
{
    lv_color_t color;     // 锁身 + 锁梁 + 锁芯统一颜色
    int       pad;        // 四周内边距
    int       thickness;  // 粗细下限（竖线/横条至少这么粗，和use/11取大）
    int       body_r;     // 锁身圆角
    bool      draw_core;  // 是否画锁芯小圆
};

static lv_obj_t *lock_widget_create(lv_obj_t *parent, int w, int h, const LockWidgetSpec &spec)
{
    // 容器：透明无样式
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_shadow_width(box, 0, 0);
    lv_obj_set_style_outline_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    // 使用区域 use_box：容器内居中的正方形
    const int use = LV_MAX(16, LV_MIN(w - spec.pad * 2, h - spec.pad * 2));
    const int bx  = spec.pad + ((w - spec.pad * 2) - use) / 2;   // use_box 左上角 x（相对容器）
    const int by  = spec.pad + ((h - spec.pad * 2) - use) / 2;   // use_box 左上角 y（相对容器）

    // 几何比例（和之前 draw_lock_shape 保持一致）
    const int beam_h = use * 40 / 100;          // 锁梁总高：40%
    const int body_h = use * 52 / 100;          // 锁身高：52%
    const int gap    = use - beam_h - body_h;   // 锁梁 - 锁身之间的缝（8%）
    const int beam_w = LV_MAX(spec.thickness, use / 11);
    const int bar_h  = beam_w;                  // 顶横条高=beam_w
    const int post_h = beam_h - bar_h;          // 竖线高=锁梁总高-顶横条

    // 竖线位置：竖线架在锁身上，左右各向内 beam_w
    const int post_left_x  = bx + beam_w;
    const int post_right_x = bx + use - beam_w * 2;
    // 锁身 x/y/w/h：占 use_box 全宽，位于 use_box 下部
    const int body_x = bx;
    const int body_w = use;
    const int body_y = by + beam_h + LV_MAX(0, gap);

    auto make_fill = [&](int x, int y, int rw, int rh, int radius) -> lv_obj_t*
    {
        lv_obj_t *o = lv_obj_create(box);
        lv_obj_remove_style_all(o);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_size(o, rw, rh);
        lv_obj_set_style_bg_color(o, spec.color, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, radius, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_shadow_width(o, 0, 0);
        lv_obj_set_style_outline_width(o, 0, 0);
        lv_obj_set_style_pad_all(o, 0, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        return o;
    };

    // ① 左竖线
    (void)make_fill(post_left_x,  by, beam_w, post_h, 0);
    // ② 右竖线
    (void)make_fill(post_right_x, by, beam_w, post_h, 0);
    // ③ 顶横条（覆盖两竖线顶端）
    (void)make_fill(post_left_x,  by, (post_right_x + beam_w) - post_left_x, bar_h, 0);
    // ④ 锁身矩形
    (void)make_fill(body_x, body_y, body_w, body_h, spec.body_r);
    // ⑤ 锁芯小圆（居中，正方形+ LV_RADIUS_CIRCLE = 圆）
    if (spec.draw_core) {
        const int core = LV_MAX(3, use / 8);
        const int cx   = bx + (use - core) / 2;
        const int cy   = body_y + body_h / 2 - core / 2;
        (void)make_fill(cx, cy, core, core, LV_RADIUS_CIRCLE);
    }

    return box;
}

// ============================================================
// AOD UI 创建 + 防烧屏抖动
// （取消时钟显示，只保留中央大锁 + 右上角小锁，
//  因为设备没有 RTC 芯片，显示假时钟没有意义，反而容易误导用户）
// ============================================================

static void aod_create_ui()
{
    lv_obj_t *screen = lv_screen_active();
    const lv_font_t *ui_font = font_manager_get_ui_font();   // 确保 font_manager 初始化过

    // AOD 全屏容器：纯黑背景覆盖正常页面（AMOLED 黑=最省功耗的底色）
    g_aod_root = lv_obj_create(screen);
    lv_obj_remove_style_all(g_aod_root);
    lv_obj_set_size(g_aod_root, FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT);
    lv_obj_set_pos(g_aod_root, 0, 0);
    lv_obj_set_style_bg_color(g_aod_root, lv_color_hex(kC_BgScreen), 0);
    lv_obj_set_style_bg_opa(g_aod_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_aod_root, 0, 0);
    lv_obj_set_style_pad_all(g_aod_root, 0, 0);
    lv_obj_add_flag(g_aod_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_aod_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_aod_root, LV_OBJ_FLAG_SCROLLABLE);

    // ====== 信息卡几何（460×460 正中） ======
    //  总内容 380 宽，垂直居中。
    //  y0 = 标题顶端 y，行间距 24。
    //  行顺序（从上往下）：
    //   [1] g_aod_title      ——  36 高、380 宽 → 主标题（Title - Artist）
    //   [2] g_aod_lyric      —— 150 高、380 宽 → 歌词（2~3 行展示为主）
    //   [3] g_aod_progress   ——   6 高、380 宽 → 细进度条
    //   [4] g_aod_time       ——  28 高、380 宽 → 时间 "mm:ss / mm:ss"
    //  总高 + 3*24 间距 = 36+150+6+28 + 3*24 = 292
    //  y0 = (460 - 292) / 2 = 84
    constexpr int kCardW      = 380;
    constexpr int kCardH_Title  = 36;
    constexpr int kCardH_Lyric  = 150;
    constexpr int kCardH_Prog   = 6;
    constexpr int kCardH_Time   = 28;
    constexpr int kGap          = 24;
    constexpr int kTotalH = kCardH_Title + kGap + kCardH_Lyric + kGap + kCardH_Prog + kGap + kCardH_Time;
    constexpr int kX0 = (FAKEPOD_LCD_WIDTH - kCardW) / 2;
    constexpr int kY0 = (FAKEPOD_LCD_HEIGHT - kTotalH) / 2;

    // 颜色（AMOLED AOD 专属：灰度低对比，不纯白，防烧屏）
    // 注意 lv_color_t 是 {red, green, blue} 多通道 uint8_t struct，不能直接塞 24bit
    // 整数字面量（会触发 -Wnarrowing），用项目统一宏 lv_color_hex()。
    const lv_color_t kC_Title  = lv_color_hex(0x9598A3);   // 中灰
    const lv_color_t kC_Lyric  = lv_color_hex(0xBFC3CD);   // 亮灰（主视觉）
    const lv_color_t kC_Time   = lv_color_hex(0x7F848F);   // 深灰
    const lv_color_t kC_ProgBg = lv_color_hex(0x2A2F3A);   // 进度条底色（接近黑）
    const lv_color_t kC_ProgFg = lv_color_hex(0xA4A8B2);   // 进度条前景（中亮灰）

    int y = kY0;

    // ---------- ① 歌曲标题-艺术家 ----------
    g_aod_title = lv_label_create(g_aod_root);
    lv_obj_set_size(g_aod_title, kCardW, kCardH_Title);
    lv_obj_set_pos(g_aod_title, kX0, y);
    lv_label_set_long_mode(g_aod_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(g_aod_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_aod_title, kC_Title, 0);
    lv_obj_set_style_text_font(g_aod_title, ui_font != nullptr ? ui_font : lv_font_default(), 0);
    lv_label_set_text(g_aod_title, "— 未在播放 —");
    lv_obj_clear_flag(g_aod_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_aod_title, LV_OBJ_FLAG_SCROLLABLE);
    y += kCardH_Title + kGap;

    // ---------- ② 当前一句歌词 ----------
    g_aod_lyric = lv_label_create(g_aod_root);
    lv_obj_set_size(g_aod_lyric, kCardW, kCardH_Lyric);
    lv_obj_set_pos(g_aod_lyric, kX0, y);
    lv_label_set_long_mode(g_aod_lyric, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g_aod_lyric, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_aod_lyric, kC_Lyric, 0);
    lv_obj_set_style_text_font(g_aod_lyric, ui_font != nullptr ? ui_font : lv_font_default(), 0);
    lv_label_set_text(g_aod_lyric, "");
    lv_obj_clear_flag(g_aod_lyric, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_aod_lyric, LV_OBJ_FLAG_SCROLLABLE);
    y += kCardH_Lyric + kGap;

    // ---------- ③ 细进度条（lv_bar 6 高）----------
    g_aod_progress = lv_bar_create(g_aod_root);
    lv_obj_set_size(g_aod_progress, kCardW, kCardH_Prog);
    lv_obj_set_pos(g_aod_progress, kX0, y);
    lv_bar_set_range(g_aod_progress, 0, 10000);   // 归一化 0~10000，避免 uint64 塞不下
    lv_bar_set_value(g_aod_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_aod_progress, kC_ProgBg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_aod_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_aod_progress, kC_ProgFg, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_aod_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_aod_progress, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(g_aod_progress, 3, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(g_aod_progress, 0, LV_PART_MAIN | LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(g_aod_progress, 0, LV_PART_MAIN | LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(g_aod_progress, 0, LV_PART_MAIN | LV_PART_INDICATOR);
    lv_obj_clear_flag(g_aod_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_aod_progress, LV_OBJ_FLAG_SCROLLABLE);
    y += kCardH_Prog + kGap;

    // ---------- ④ 时间文字 ----------
    g_aod_time = lv_label_create(g_aod_root);
    lv_obj_set_size(g_aod_time, kCardW, kCardH_Time);
    lv_obj_set_pos(g_aod_time, kX0, y);
    lv_label_set_long_mode(g_aod_time, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(g_aod_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_aod_time, kC_Time, 0);
    lv_obj_set_style_text_font(g_aod_time, ui_font != nullptr ? ui_font : lv_font_default(), 0);
    lv_label_set_text(g_aod_time, "00:00 / 00:00");
    lv_obj_clear_flag(g_aod_time, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_aod_time, LV_OBJ_FLAG_SCROLLABLE);

    // ---------- 右上角小锁（提醒已锁定，不抢主视觉） ----------
    {
        LockWidgetSpec tiny_spec = {};
        tiny_spec.color     = lv_color_hex(0x555555);
        tiny_spec.pad       = 4;
        tiny_spec.thickness = 2;
        tiny_spec.body_r    = 3;
        tiny_spec.draw_core = true;
        g_aod_tip_lock = lock_widget_create(g_aod_root, 32, 32, tiny_spec);
        lv_obj_align(g_aod_tip_lock, LV_ALIGN_TOP_RIGHT, -36, 36);
    }

    // ---------- Normal 页右上角的小锁 ----------（和以前一致，不改动）
    {
        lv_obj_t *cap = lv_obj_create(screen);
        lv_obj_remove_style_all(cap);
        lv_obj_set_size(cap, 44, 44);
        lv_obj_set_style_bg_color(cap, lv_color_hex(kC_BgScreen), 0);
        lv_obj_set_style_bg_opa(cap, LV_OPA_80, 0);
        lv_obj_set_style_radius(cap, 14, 0);
        lv_obj_set_style_pad_all(cap, 0, 0);
        lv_obj_set_style_border_width(cap, 0, 0);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(cap, LV_ALIGN_TOP_RIGHT, -18, 18);

        LockWidgetSpec small_spec = {};
        small_spec.color     = lv_color_hex(0xCCCCCC);
        small_spec.pad       = 4;
        small_spec.thickness = 3;
        small_spec.body_r    = 4;
        small_spec.draw_core = true;
        g_lock_icon = lock_widget_create(cap, 32, 32, small_spec);
        lv_obj_center(g_lock_icon);
    }
    obj_set_visible(lv_obj_get_parent(g_lock_icon), false);   // 初始隐藏整个胶囊（未锁定状态）
}

// 30s ±1px 抖动防烧屏（对 AOD 的 5 个文字/进度 + 角落小锁整体偏移）
// 所有元素的"基础坐标"保持一致（kX0/kY0/对齐锚点），只是在基础上加上 (jx, jy) 再摆放。
static void aod_apply_jitter_if_due(const TickType_t now)
{
    if (g_aod_root == nullptr || g_power != ScreenPowerAOD) return;
    if (g_last_jitter_tick != 0 &&
        (now - g_last_jitter_tick) < pdMS_TO_TICKS(kJitterIntervalMs)) {
        return;
    }
    g_last_jitter_tick = now;

    // 伪随机 ±1px
    const uint32_t t = static_cast<uint32_t>(now);
    int8_t jx = static_cast<int8_t>(((t >> 3) % 3U) - 1U);
    int8_t jy = static_cast<int8_t>(((t >> 7) % 3U) - 1U);
    if (jx >  kJitterMaxPx) jx =  kJitterMaxPx;
    if (jx < -kJitterMaxPx) jx = -kJitterMaxPx;
    if (jy >  kJitterMaxPx) jy =  kJitterMaxPx;
    if (jy < -kJitterMaxPx) jy = -kJitterMaxPx;
    g_jitter_x = jx;
    g_jitter_y = jy;

    // AOD 内部 4 行：它们的基础坐标就是 (kX0, kY0 + 各偏移)，这里直接把整体 (jx, jy)
    //     叠加到每个控件上 —— 保持行间相对位置不变。
    constexpr int kCardW       = 380;
    constexpr int kCardH_Title = 36;
    constexpr int kCardH_Lyric = 150;
    constexpr int kCardH_Prog  = 6;
    constexpr int kCardH_Time  = 28;
    constexpr int kGap         = 24;
    constexpr int kTotalH = kCardH_Title + kGap + kCardH_Lyric + kGap + kCardH_Prog + kGap + kCardH_Time;
    constexpr int kX0 = (FAKEPOD_LCD_WIDTH - kCardW) / 2;
    constexpr int kY0 = (FAKEPOD_LCD_HEIGHT - kTotalH) / 2;

    int y = kY0;
    if (g_aod_title)    lv_obj_set_pos(g_aod_title,    kX0 + jx, y + jy);
    y += kCardH_Title + kGap;
    if (g_aod_lyric)    lv_obj_set_pos(g_aod_lyric,    kX0 + jx, y + jy);
    y += kCardH_Lyric + kGap;
    if (g_aod_progress) lv_obj_set_pos(g_aod_progress, kX0 + jx, y + jy);
    y += kCardH_Prog + kGap;
    if (g_aod_time)     lv_obj_set_pos(g_aod_time,     kX0 + jx, y + jy);

    // 右上角小锁：基础对齐 TOP_RIGHT (x=-36, y=36)
    if (g_aod_tip_lock != nullptr) {
        lv_obj_align(g_aod_tip_lock, LV_ALIGN_TOP_RIGHT,
            -36 + jx, 36 + jy);
    }
}

// ============================================================
// AOD 周期刷新（1s 一次）：标题/歌词/进度/时间
// ============================================================
static void aod_format_ms(char *out, size_t out_size, uint64_t ms)
{
    if (out == nullptr || out_size == 0U) return;
    uint64_t total_sec = ms / 1000U;
    unsigned m = static_cast<unsigned>(total_sec / 60U);
    unsigned s = static_cast<unsigned>(total_sec % 60U);
    snprintf(out, out_size, "%02u:%02u", m, s);
}

static void aod_refresh_now_playing_if_due(const TickType_t now_tick)
{
    if (g_aod_root == nullptr || g_power != ScreenPowerAOD) return;

    const uint32_t now_ms = static_cast<uint32_t>(pdTICKS_TO_MS(now_tick));
    if (g_aod_last_refresh_tick != 0U &&
        (now_ms - g_aod_last_refresh_tick) < kAODRefreshTickMs) {
        return;
    }
    g_aod_last_refresh_tick = now_ms;

    // 1) 播放快照（位置/总时长/曲目索引）
    AudioStateSnapshot snap = {};
    (void)audio_service_get_snapshot(&snap);

    // 2) 曲目信息（title / artist）
    const size_t list_cnt = player_state_get_list_count();
    const size_t idx      = player_state_get_index();
    bool    song_changed = false;

    if (list_cnt == 0U || idx >= list_cnt) {
        if (g_aod_last_track_idx != UINT32_MAX) {
            lv_label_set_text(g_aod_title, "— 未在播放 —");
            lv_label_set_text(g_aod_lyric, "");
            g_aod_last_track_idx      = UINT32_MAX;
            g_aod_last_lyric_line_idx = UINT32_MAX;
            song_changed = true;
        }
    } else {
        MediaTrackViewV2 view = {};
        const bool have_view = media_catalog_v2_get_track_view(idx, &view);

        char title_buf[512] = {};
        const char *title = nullptr;
        if (have_view && view.title != nullptr && view.title[0]) {
            title = view.title;
        } else if (media_library_copy_display_name(idx, title_buf, sizeof(title_buf))) {
            title = title_buf;
        } else {
            snprintf(title_buf, sizeof(title_buf), "歌曲 %u", static_cast<unsigned>(idx + 1U));
            title = title_buf;
        }
        const char *artist = (have_view && view.artist != nullptr && view.artist[0])
            ? view.artist : "未知歌手";

        char comb[640] = {};
        snprintf(comb, sizeof(comb), "%s  -  %s", title, artist);

        if (g_aod_last_track_idx != idx) {
            lv_label_set_text(g_aod_title, comb);
            g_aod_last_track_idx = idx;
            song_changed = true;
            // 新曲第一次显示：主动向 LyricsService 提交一次加载请求。
            // （否则歌词只在用户进入过歌词页才会被 LyricsTask 从 TF 卡取出，
            //   直接 AOD 的话一直拿不到 → 显示"暂无歌词"。）
            (void)lyrics_service_request_track(static_cast<uint32_t>(idx));
            // 新曲强制刷新歌词行（避免沿用旧曲的"当前行索引"）
            g_aod_last_lyric_line_idx = UINT32_MAX;
        }
    }

    // 3) 歌词（找 window.lines 中 current=true 的那行）
    {
        LyricsWindowSnapshot win = {};
        const char *lyric_text   = nullptr;
        uint32_t     curr_line   = UINT32_MAX;
        bool         have_curr   = false;

        if (g_aod_last_track_idx != UINT32_MAX &&
            lyrics_service_get_window(
                static_cast<uint32_t>(g_aod_last_track_idx),
                static_cast<uint32_t>(snap.position_ms), &win)) {
            for (size_t i = 0; i < LYRICS_VIEW_WINDOW_LINES; ++i) {
                if (win.lines[i].valid && win.lines[i].current) {
                    lyric_text = win.lines[i].text;
                    curr_line  = win.current_line_index != UINT32_MAX
                        ? win.current_line_index : static_cast<uint32_t>(i);
                    have_curr  = true;
                    break;
                }
            }
        }

        if (song_changed || g_aod_last_lyric_line_idx != curr_line) {
            if (!have_curr || lyric_text == nullptr || lyric_text[0] == '\0') {
                // 没歌词：显示"暂无歌词"，避免一大片空白
                lv_label_set_text(g_aod_lyric, "（暂无歌词）");
            } else {
                lv_label_set_text(g_aod_lyric, lyric_text);
            }
            g_aod_last_lyric_line_idx = curr_line;
        }
    }

    // 4) 时间文字 + 进度条（归一化 0~10000）
    //    total_ms 来源优先级：
    //      ① audio_service 快照字段里没直接给 total_duration_ms，
    //        但提供 total_frames / sample_rate_hz，可自己推（真实播放帧 = 真值）
    //      ② media_library_get_technical_info.duration_ms（文件扫描时缓存的值）
    //      ③ 0（显示 00:00）
    uint64_t pos_ms   = snap.position_ms;
    uint64_t total_ms = 0U;
    if (snap.sample_rate_hz > 0U && snap.total_frames > 0U) {
        // Xtensa GCC 无 __uint128_t。用 uint64_t：
        //   单首歌最长 24h @192kHz ≈ 16.6B 帧
        //   frames*1000 ≈ 1.66e13 << 2^64 (1.84e19)，不会溢出
        total_ms = static_cast<uint64_t>(
            (static_cast<uint64_t>(snap.total_frames) * 1000ULL) / snap.sample_rate_hz);
    }
    if (total_ms == 0U && g_aod_last_track_idx != UINT32_MAX) {
        MediaTechnicalInfo ti = {};
        if (media_library_get_technical_info(
                static_cast<size_t>(g_aod_last_track_idx), &ti) && ti.duration_ms > 0U) {
            total_ms = ti.duration_ms;
        }
    }
    if (total_ms != 0U && pos_ms > total_ms) pos_ms = total_ms;

    if (g_aod_last_pos_ms != pos_ms || g_aod_last_total_ms != total_ms) {
        char cur[16] = {}; aod_format_ms(cur, sizeof(cur),   pos_ms);
        char tot[16] = {}; aod_format_ms(tot, sizeof(tot), total_ms ? total_ms : 0U);
        char tmb[48] = {};
        snprintf(tmb, sizeof(tmb), "%s  /  %s", cur, tot);
        if (g_aod_time) lv_label_set_text(g_aod_time, tmb);

        if (g_aod_progress) {
            const int32_t pct = (total_ms == 0U) ? 0 :
                static_cast<int32_t>((pos_ms * 10000ULL) / total_ms);
            lv_bar_set_value(g_aod_progress, LV_CLAMP(pct, 0, 10000), LV_ANIM_OFF);
        }

        g_aod_last_pos_ms   = pos_ms;
        g_aod_last_total_ms = total_ms;
    }
}

// ============================================================
// 屏态切换
// ============================================================
static void apply_power_hw(ScreenPowerState next)
{
    switch (next) {
    case ScreenPowerNormal:
        display_panel_apply_brightness(kBrightnessNormal);
        display_panel_apply_output(true);
        break;
    case ScreenPowerAOD:
        display_panel_apply_output(true);
        display_panel_apply_brightness(kBrightnessAOD);
        break;
    case ScreenPowerOff:
        display_panel_apply_brightness(kBrightnessOff);
        display_panel_apply_output(false);
        break;
    }
}

// ----- 前置声明（apply_power_ui 需要调 apply_lock_ui，后者定义在后面）-----
static void apply_lock_ui();

// 把屏幕上所有"非我们自己创建的"音乐前台页面统一后台化/关闭：
//   - Music 主页：player_home_app_leave_background()   暂停前台活动，释放 Artwork UI lease
//   - 歌词页：  lyrics_view_close()
//   - 频谱页：  spectrum_view_close()
//   - 曲库页：  library_view_suspend_for_app_switch()
// 这样 AOD 独立页下面不再有任何其他可见页面 → 切歌时 player_home 已经后台化，
// 不会再 move_foreground(封面) → 彻底杜绝"封面从 AOD 底下露出来"。
static void aod_push_music_pages_to_background()
{
    // 先保存退出时要怎么还原
    g_aod_prev_music_foreground  = player_home_app_is_foreground();
    g_aod_prev_library_visible   = library_view_is_visible();

    // 注意：library/lyrics/spectrum 三者互斥（一个可见另两个必 HIDDEN），
    // 所以 g_aod_prev_was_fullscreen = 这三个里有任何一个是可见的即可。
    // lyrics_view / spectrum_view 没有暴露 is_visible，但我们既然要关闭就
    // 用"library 可见 + Music 不在前台"这两个信号来判断 —— 因为当歌词/频谱
    // 打开时，player_home_app_is_foreground() 一定是 true（它们都在 Music App
    // 内），但 library 不在 Music App，走 library_view_is_visible。
    // 对于歌词/频谱，我们统一用 "player_home_app_is_foreground == true
    // 且 overlay/launcher 都不可见" 作为判断太耦合。
    // 更简单：三页 close 都幂等（已经关的再关无害），所以直接调。
    (void)g_aod_prev_library_visible;   // 显式避免 unused（退出时用得上）

    spectrum_view_close();
    lyrics_view_close();
    library_view_suspend_for_app_switch();
    if (player_home_app_is_foreground()) {
        (void)player_home_app_leave_background();   // 仅 Music 在前台时才 leave
    }
    // g_aod_prev_was_fullscreen 保守策略：用户如果不是在主页就是全屏页。
    //   只要 library 可见，就判定为"在曲库"；否则 Music 是前台，且我们上面
    //   已经 leave_background 把主页藏起来了 → 默认当作 fullscreen。
    g_aod_prev_was_fullscreen = library_view_is_visible();
    // 上面 library_view_suspend 已经把曲库 HIDDEN 了，所以要用保存前的值
    g_aod_prev_was_fullscreen = g_aod_prev_library_visible;
}

// 退出 AOD → 按保存的线索恢复原前台页面
static void aod_restore_music_pages_from_background()
{
    // 顺序：先让 Music App 回到前台（player_home 恢复），
    // 然后如果原来停在曲库，再把曲库 open；如果原来停在 Music 内部
    // 全屏页（歌词/频谱），player_home_app_enter_foreground() 不会自动
    // 还原（因为 close 时就销毁了），此时行为等价于回到播放器主界面，
    // 这是合理的（歌词/频谱在 AOD 唤醒后用户再打开即可）。
    if (g_aod_prev_music_foreground) {
        (void)player_home_app_enter_foreground();
    }
    // 如果进入 AOD 之前用户在曲库 → 重新打开曲库
    if (g_aod_prev_library_visible) {
        library_view_open();
    } else if (!g_aod_prev_was_fullscreen) {
        // 进入 AOD 前就是在播放器主页 → 确保 Artwork/按钮 lease 等全部回来
        player_home_resume_from_fullscreen_view("AOD退出回到播放器主页");
    }
    // 否则 g_aod_prev_was_fullscreen == true 但不是曲库 = 歌词/频谱（已关） →
    // 用户需要手动重开，和 library_view_suspend_for_app_switch 的行为一致。
}

static void apply_power_ui(ScreenPowerState prev, ScreenPowerState next)
{
    // ── AOD "独立页面"模式：进入时把音乐页全后台，退出时按线索恢复 ──
    const bool was_aod = (prev == ScreenPowerAOD);
    const bool to_aod  = (next == ScreenPowerAOD);

    if (!was_aod && to_aod) {
        // 第一次切进 AOD：保存 + 关页面 + 信息卡立即刷新 + 置顶一次
        aod_push_music_pages_to_background();
        obj_set_visible(g_aod_root, true);
        lv_obj_move_foreground(g_aod_root);
        g_aod_last_refresh_tick = 0U;
        g_last_jitter_tick      = 0;
        const TickType_t now    = xTaskGetTickCount();
        aod_refresh_now_playing_if_due(now);
        aod_apply_jitter_if_due(now);
    } else if (was_aod && !to_aod) {
        // 从 AOD 切到 Normal 或 Off：隐藏 AOD + 恢复原页面
        obj_set_visible(g_aod_root, false);
        if (next == ScreenPowerNormal) {
            aod_restore_music_pages_from_background();
            // 恢复后立即把 Normal 右上角锁（如已锁）置顶
            apply_lock_ui();
        }
    } else if (to_aod) {
        // AOD → AOD（保持态）：如果切歌后信息卡曾被任何兄弟盖过，这里再提一次顶
        // （正常 AOD->AOD 不会走这里，只是 defensive，防止别人中途改状态）
        if (g_aod_root != nullptr) lv_obj_move_foreground(g_aod_root);
    } else {
        // Normal <-> Off 等其他切换：保持右上角 [锁] 置顶
        if (g_lock_icon != nullptr) {
            lv_obj_t *cap = lv_obj_get_parent(g_lock_icon);
            if (cap != nullptr) lv_obj_move_foreground(cap);
            else                lv_obj_move_foreground(g_lock_icon);
        }
    }
}

static void apply_lock_ui()
{
    if (g_lock_icon == nullptr) return;
    const bool vis = (g_lock == ScreenLockLocked) && (g_power == ScreenPowerNormal);
    // g_lock_icon 是在一个 44×44 胶囊底板里面的 32×32 绘制对象，
    // 显隐时操作外层底板（否则胶囊背景还露出来）。
    lv_obj_t *cap = lv_obj_get_parent(g_lock_icon);
    if (cap != nullptr) {
        obj_set_visible(cap, vis);
        if (vis) lv_obj_move_foreground(cap);
    } else {
        obj_set_visible(g_lock_icon, vis);
        if (vis) lv_obj_move_foreground(g_lock_icon);
    }
}

// ============================================================
// 菜单 UI + 行点击回调
// ============================================================
static void execute_selected_row(ScreenActionRow row);   // 前置定义

static void menu_apply_highlight_visuals()
{
    for (int i = 0; i < static_cast<int>(ScreenActionRowCount); ++i) {
        lv_obj_t *row = g_menu_rows[i];
        if (row == nullptr) continue;
        const bool hi = (i == static_cast<int>(g_menu_highlight));
        const lv_color_t bg    = lv_color_hex(hi ? kC_RowSelBg : kC_RowUnselBg);
        const lv_opa_t    bgop = hi ? kC_RowSelBgOpa : kC_RowUnselBgOpa;
        const lv_color_t tc    = lv_color_hex(hi ? kC_RowSelText : kC_RowUnselText);
        lv_obj_set_style_bg_color(row, bg, 0);
        lv_obj_set_style_bg_opa(row,   bgop, 0);
        // 行容器本身的文字色（对子 label 的继承兜底）
        lv_obj_set_style_text_color(row, tc, 0);
        // 子 label（每行第一个 child）：直接覆盖样式，确保文字颜色切换
        lv_obj_t *label = lv_obj_get_child(row, 0);
        if (label != nullptr) {
            lv_obj_set_style_text_color(label, tc, 0);
        }
    }
}

static void menu_create_ui()
{
    lv_obj_t *screen = lv_screen_active();
    (void)font_manager_get_ui_font();   // 确保 font_manager 初始化过

    // =======================================
    // g_menu_root：全屏悬浮层，完全透明 + FLOATING（不透出底下画面的任何视觉）
    // =======================================
    g_menu_root = lv_obj_create(screen);
    lv_obj_remove_style_all(g_menu_root);
    lv_obj_set_size(g_menu_root, FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT);
    lv_obj_set_pos(g_menu_root, 0, 0);
    lv_obj_set_style_bg_opa(g_menu_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(g_menu_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(g_menu_root, 0, 0);
    lv_obj_set_style_shadow_width(g_menu_root, 0, 0);
    lv_obj_set_style_outline_width(g_menu_root, 0, 0);
    lv_obj_set_style_pad_all(g_menu_root, 0, 0);
    lv_obj_add_flag(g_menu_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_menu_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_menu_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_menu_root, LV_OBJ_FLAG_FLOATING);

    // =======================================
    // g_menu_panel = nullptr（已删除卡片面板"黑色方背景"）。
    // 直接把 4 个胶囊行列在 460×460 正中。
    // 坐标常量使用全局的 kMENU_ROW_WIDTH / kMENU_ROW_GAP / kMENU_ROW_XLEFT /
    //                    kMENU_BLOCK_TOP（和 row_screen_rect_y 命中矩形函数完全一致，
    //                    避免两处计算不一致导致触摸命中偏移）
    // =======================================
    g_menu_panel = nullptr;

    for (int i = 0; i < static_cast<int>(ScreenActionRowCount); ++i) {
        lv_obj_t *row = lv_obj_create(g_menu_root);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, kMENU_ROW_WIDTH, kMenuRowHeight);
        const int y_top = kMENU_BLOCK_TOP + i * (kMenuRowHeight + kMENU_ROW_GAP);
        lv_obj_set_pos(row, kMENU_ROW_XLEFT, y_top);

        // -------- 胶囊底板样式（和主页按钮同款，创建时写死，menu_apply_highlight_visuals
        //          只负责切换 "选中 vs 未选中"两档的 bg_opa 与 文字色）--------
        lv_obj_set_style_radius(row, kMenuRowRadius, 0);        // LV_RADIUS_CIRCLE = 胶囊
        lv_obj_set_style_pad_left(row, 0, 0);
        lv_obj_set_style_pad_right(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_outline_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        // 未选中默认：白底 + opa 32（和首页 play/prev/next 按钮一致）
        lv_obj_set_style_bg_color(row, lv_color_hex(kC_RowUnselBg), 0);
        lv_obj_set_style_bg_opa(row, kC_RowUnselBgOpa, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, kRowText[i]);
        const lv_font_t *ui_font = font_manager_get_ui_font();
        lv_obj_set_style_text_font(label, ui_font != nullptr ? ui_font : lv_font_default(), 0);
        // 未选中文字：#F5F7FA（和主页 pill 按钮文字同色）
        lv_obj_set_style_text_color(label, lv_color_hex(kC_RowUnselText), 0);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(label);

        g_menu_rows[i] = row;
    }

    // 再跑一次高亮视觉：第 0 行默认选中（bg_opa=90 + 白字），其他保持上面的未选中
    menu_apply_highlight_visuals();
}

static void execute_selected_row(ScreenActionRow row)
{
    ESP_LOGI(TAG, "动作菜单：执行 行%u[%s]",
        static_cast<unsigned>(row), kRowText[static_cast<int>(row)]);
    switch (row) {
    case ScreenActionRowToggleLock:
        screen_lock_toggle_lock();
        break;
    case ScreenActionRowAOD:
        screen_lock_set_power(ScreenPowerAOD);
        break;
    case ScreenActionRowScreenOff:
        screen_lock_set_power(ScreenPowerOff);
        break;
    case ScreenActionRowCancel:
    default:
        break;
    }
}

// ============================================================
// 菜单：行矩形命中（纯坐标，不依赖 LVGL 对象的 CLICKABLE/事件）
// 位置常量来自顶部的 kMENU_ROW_WIDTH / kMENU_ROW_GAP / kMENU_BLOCK_TOP / kMENU_ROW_XLEFT
// （与 menu_create_ui 布局同源，确保视觉/触摸一致，命中偏移=0）
// ============================================================

static bool row_screen_rect_y(int idx, int *y1_out, int *y2_out)
{
    if (idx < 0 || idx >= static_cast<int>(ScreenActionRowCount)) return false;
    const int y1 = kMENU_BLOCK_TOP + idx * (kMenuRowHeight + kMENU_ROW_GAP);
    if (y1_out) *y1_out = y1;
    if (y2_out) *y2_out = y1 + kMenuRowHeight;
    return true;
}

static int hit_row_index(int y)
{
    for (int i = 0; i < static_cast<int>(ScreenActionRowCount); ++i) {
        int y1 = 0, y2 = 0;
        if (!row_screen_rect_y(i, &y1, &y2)) continue;
        if (y >= y1 && y <= y2) return i;
    }
    return -1;
}

bool screen_action_menu_on_touch_press(int x, int y)
{
    (void)x;
    if (!g_menu_open) return false;
    const int idx = hit_row_index(y);
    g_press_x = x;
    g_press_y = y;
    g_press_row = idx; // -1 表示卡片外按的（UP 不执行）
    if (idx >= 0) {
        screen_action_menu_set_highlight(static_cast<ScreenActionRow>(idx));
    }
    return true; // 不管内外都吞掉（卡片外点击忽略，也不传到底下页面）
}

bool screen_action_menu_on_touch_release(int x, int y)
{
    (void)x;
    if (!g_menu_open) return false;
    const int release_idx = hit_row_index(y);
    const int pressed     = g_press_row;
    g_press_row = -1;
    // 按下和释放在同一行，且不是卡片外 → 执行
    if (pressed >= 0 && release_idx == pressed) {
        ESP_LOGI(TAG, "动作菜单：按下行%u 释放行%u → 匹配，执行[%s]",
            (unsigned)pressed, (unsigned)release_idx,
            kRowText[pressed]);
        screen_action_menu_close(true);
    } else {
        ESP_LOGI(TAG, "动作菜单：行不匹配 press=%d release=%d → 不执行（取消由GPIO0长按）",
            pressed, release_idx);
    }
    return true;
}

// ============================================================
// 公开 API
// ============================================================

esp_err_t screen_lock_simple_create()
{
    if (g_ready) return ESP_OK;

    g_power = ScreenPowerNormal;
    g_lock  = ScreenLockUnlocked;
    g_last_jitter_tick = 0;
    g_press_row        = -1;
    g_press_x = 0; g_press_y = 0;

    if (!lvgl_port_lock(200)) {
        ESP_LOGW(TAG, "screen_lock_simple_create 获取 LVGL 锁超时");
        return ESP_ERR_TIMEOUT;
    }

    aod_create_ui();
    menu_create_ui();
    apply_power_ui(ScreenPowerNormal, ScreenPowerNormal);
    apply_lock_ui();
    lvgl_port_unlock();

    g_ready = true;
    g_menu_open = false;
    g_menu_highlight = ScreenActionRowToggleLock;
    ESP_LOGI(TAG, "screen_lock_simple 创建完成：Normal + Unlocked（AOD 不显示时钟）");
    return ESP_OK;
}

void screen_lock_simple_destroy()
{
    if (!g_ready) return;
    if (!lvgl_port_lock(500)) {
        ESP_LOGW(TAG, "screen_lock_simple_destroy LVGL 锁超时，跳过清理");
        return;
    }
    if (g_aod_root)        { lv_obj_delete(g_aod_root); }
    if (g_lock_icon)       {
        lv_obj_t *cap = lv_obj_get_parent(g_lock_icon);
        if (cap != nullptr) lv_obj_delete(cap);
        else                lv_obj_delete(g_lock_icon);
    }
    if (g_menu_root)       { lv_obj_delete(g_menu_root); }
    g_aod_root        = nullptr;
    g_aod_title       = nullptr;
    g_aod_lyric       = nullptr;
    g_aod_progress    = nullptr;
    g_aod_time        = nullptr;
    g_aod_tip_lock    = nullptr;
    g_aod_last_track_idx       = UINT32_MAX;
    g_aod_last_lyric_line_idx  = UINT32_MAX;
    g_aod_last_pos_ms          = UINT64_MAX;
    g_aod_last_total_ms        = UINT64_MAX;
    g_aod_last_refresh_tick    = 0U;
    g_lock_icon       = nullptr;
    g_menu_root       = nullptr;
    g_menu_panel      = nullptr;
    for (auto &p : g_menu_rows) p = nullptr;
    g_menu_tip        = nullptr;
    g_menu_open       = false;
    lvgl_port_unlock();
    g_ready = false;
}

// ----------- 查询（老 + 新命名都提供） -----------
ScreenPowerState screen_lock_simple_get_power(void) { return g_power; }
ScreenLockState  screen_lock_simple_get_lock (void) { return g_lock;  }
bool             screen_lock_is_locked       (void) { return g_lock == ScreenLockLocked; }
ScreenPowerState screen_lock_get_power       (void) { return g_power; }  // 老别名
ScreenLockState  screen_lock_get_lock        (void) { return g_lock;  }  // 老别名

bool screen_lock_should_block_touch(void)
{
    if (g_menu_open) return false;              // 菜单打开 → 放行给 LVGL 点击行卡片
    if (g_power == ScreenPowerOff) return true; // 熄屏：拦截
    return g_lock == ScreenLockLocked;          // 锁定：拦截（仅 AOD/Normal 生效）
}

// ----------- 切换 -----------
void screen_lock_simple_set_power(ScreenPowerState power)
{
    if (!g_ready) return;
    const ScreenPowerState prev = g_power;
    if (prev == power) return;
    g_power = power;
    if (power == ScreenPowerAOD) {              // 进入 AOD 自动锁定
        g_lock = ScreenLockLocked;
    }

    if (!lvgl_port_lock(200)) {
        ESP_LOGW(TAG, "screen_lock_set_power LVGL 锁超时");
    } else {
        apply_power_ui(prev, power);
        apply_lock_ui();
        lvgl_port_unlock();
    }
    apply_power_hw(power);
    ESP_LOGI(TAG, "屏态切换：%u -> %u，锁定=%s",
        (unsigned)prev, (unsigned)power,
        g_lock == ScreenLockLocked ? "YES" : "NO");
}

void screen_lock_simple_set_lock(ScreenLockState lock)
{
    if (!g_ready) return;
    if (lock == ScreenLockUnlocked && g_power != ScreenPowerNormal) {
        screen_lock_simple_set_power(ScreenPowerNormal);
    }
    if (g_lock == lock) return;
    g_lock = lock;

    if (!lvgl_port_lock(200)) {
        ESP_LOGW(TAG, "screen_lock_set_lock LVGL 锁超时");
    } else {
        apply_lock_ui();
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "锁状态切换：%s",
        g_lock == ScreenLockLocked ? "LOCKED" : "UNLOCKED");
}

void screen_lock_set_power(ScreenPowerState power) { screen_lock_simple_set_power(power); } // 老别名
void screen_lock_set_lock (ScreenLockState  lock)  { screen_lock_simple_set_lock(lock);  } // 老别名

void screen_lock_toggle_lock(void)
{
    screen_lock_simple_set_lock(
        g_lock == ScreenLockLocked ? ScreenLockUnlocked : ScreenLockLocked);
}

void screen_lock_wake_if_needed(void)
{
    if (g_power == ScreenPowerNormal) return;
    screen_lock_simple_set_power(ScreenPowerNormal);
}

// ----------- Render -----------
static bool screen_lock_simple_render_due(TickType_t now)
{
    if (g_power == ScreenPowerAOD) {
        if (g_aod_root == nullptr) return false;

        const uint32_t now_ms = static_cast<uint32_t>(pdTICKS_TO_MS(now));
        const bool refresh_due = g_aod_last_refresh_tick == 0U ||
            (now_ms - g_aod_last_refresh_tick) >= kAODRefreshTickMs;
        const bool jitter_due = g_last_jitter_tick == 0 ||
            (now - g_last_jitter_tick) >= pdMS_TO_TICKS(kJitterIntervalMs);
        return refresh_due || jitter_due;
    }

    // Normal + Locked 仍保持原来的 10ms 置顶兜底；Normal + Unlocked / Off 无渲染工作。
    return g_power == ScreenPowerNormal &&
        g_lock == ScreenLockLocked && g_lock_icon != nullptr;
}

void screen_lock_simple_render(void)
{
    if (!g_ready) return;
    const TickType_t now = xTaskGetTickCount();
    if (!screen_lock_simple_render_due(now)) return;
    if (!lvgl_port_lock(10)) return;

    // 防烧屏抖动（AOD，30s ±1px）
    aod_apply_jitter_if_due(now);
    // AOD 信息 200ms 刷新（歌名/歌词/进度条）—— 和 AudioTask 250ms 快照同步
    aod_refresh_now_playing_if_due(now);

    // AOD 独立页面模式：不再需要每轮抢顶层。4 个音乐页已经被 leave_background，
    // AOD 下面没有任何兄弟层可见，不可能再发生"切歌封面露头"。

    // Normal + Locked：右上角 [锁] 胶囊保持在最顶层（和之前逻辑相同）
    if (g_lock == ScreenLockLocked && g_power == ScreenPowerNormal &&
        g_lock_icon != nullptr) {
        lv_obj_t *cap = lv_obj_get_parent(g_lock_icon);
        if (cap != nullptr) lv_obj_move_foreground(cap);
        else                lv_obj_move_foreground(g_lock_icon);
    }
    lvgl_port_unlock();
}

// ----------- 锁图标显式重绘 -----------
void screen_lock_simple_invalidate_lock_icon(void)
{
    if (!g_ready) return;
    // 只有 Normal + Locked 才有需要重绘的右上角 [锁]
    if (g_lock != ScreenLockLocked || g_power != ScreenPowerNormal) return;
    if (g_lock_icon == nullptr) return;
    if (!lvgl_port_lock(10)) return;

    // BoundedSPI 封面直写整屏覆盖了锁图标：重新标脏 + 置顶，让下一帧 LVGL flush 画回来
    lv_obj_t *cap = lv_obj_get_parent(g_lock_icon);
    lv_obj_invalidate(g_lock_icon);
    if (cap != nullptr) {
        lv_obj_move_foreground(cap);
        lv_obj_invalidate(cap);
    } else {
        lv_obj_move_foreground(g_lock_icon);
    }
    lvgl_port_unlock();
}

// ----------- 菜单 API -----------
void screen_action_menu_set_highlight(ScreenActionRow row)
{
    int r = static_cast<int>(row);
    if (r < 0) r = 0;
    if (r >= static_cast<int>(ScreenActionRowCount))
        r = static_cast<int>(ScreenActionRowCount) - 1;
    g_menu_highlight = static_cast<ScreenActionRow>(r);
    if (!g_ready || !g_menu_open) return;
    if (!lvgl_port_lock(20)) return;
    menu_apply_highlight_visuals();
    lvgl_port_unlock();
}

void screen_action_menu_open(void)
{
    if (!g_ready) return;
    // 长按 GPIO0 进入菜单 → 如果屏在暗态，先亮回 Normal 再打开
    // （gpio0_service 在暗态长按已直接解锁流程回了，这里做兜底保持一致）
    if (g_power != ScreenPowerNormal) {
        screen_lock_wake_if_needed();
    }
    if (g_menu_open) return;
    if (!lvgl_port_lock(200)) {
        ESP_LOGW(TAG, "动作菜单打开：LVGL 锁超时");
        return;
    }
    g_menu_highlight = ScreenActionRowToggleLock; // 默认选中「锁屏」
    g_press_row = -1;
    menu_apply_highlight_visuals();
    if (g_menu_root != nullptr) {
        lv_obj_move_foreground(g_menu_root);
        obj_set_visible(g_menu_root, true);
    }
    g_menu_open = true;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "动作菜单打开（松手后保留；点击行执行；再次长按GPIO0可退出）");
}

void screen_action_menu_close(bool execute_if_valid)
{
    if (!g_ready || !g_menu_open) return;
    const ScreenActionRow sel = g_menu_highlight;
    const bool do_exec = execute_if_valid && (sel != ScreenActionRowCancel);
    // 先关菜单再执行，避免执行熄屏时被 LVGL 再次刷新
    if (!lvgl_port_lock(100)) {
        g_menu_open = false;
        if (do_exec) execute_selected_row(sel);
        return;
    }
    if (g_menu_root != nullptr) obj_set_visible(g_menu_root, false);
    g_menu_open = false;
    g_press_row = -1;
    lvgl_port_unlock();

    if (do_exec) execute_selected_row(sel);
    ESP_LOGI(TAG, "动作菜单关闭（execute=%s sel=%s）",
        do_exec ? "YES" : "NO",
        kRowText[static_cast<int>(sel)]);
}

bool screen_action_menu_is_open(void) { return g_menu_open; }

// ----------- 老签名兼容（内部转新 press/release 模型，保留但不再推荐） -----------
bool screen_action_menu_handle_touch_down(int x, int y)
{
    return screen_action_menu_on_touch_press(x, y);
}
void screen_action_menu_handle_touch_up(void)
{
    // 老签名拿不到 release 坐标，就用按下坐标推断（99% 情况下就是同一行命中）
    if (!g_menu_open) return;
    if (g_press_row < 0) return;
    const int idx = g_press_row;
    g_press_row = -1;
    ESP_LOGI(TAG, "动作菜单：老UP路径执行 行%u[%s]",
        (unsigned)idx, kRowText[idx]);
    screen_action_menu_close(true);
}

bool screen_action_menu_handle_touch(int x, int y)
{
    return screen_action_menu_on_touch_press(x, y);
}
void screen_action_menu_handle_drag(int y)
{
    if (!g_menu_open) return;
    const int idx = hit_row_index(y);
    if (idx >= 0 && static_cast<ScreenActionRow>(idx) != g_menu_highlight) {
        screen_action_menu_set_highlight(static_cast<ScreenActionRow>(idx));
    }
}
void screen_action_menu_confirm_and_close(void)
{
    if (!g_menu_open) return;
    screen_action_menu_close(true);
}

// ----------- 层级置顶 -----------
void screen_lock_simple_raise(void)
{
    if (!g_ready) return;
    if (!lvgl_port_lock(50)) return;
    // 顺序：小锁图标(连同胶囊底板) → AOD 全屏 → 菜单层（菜单最顶）
    if (g_lock_icon != nullptr) {
        lv_obj_t *cap = lv_obj_get_parent(g_lock_icon);
        if (cap != nullptr) lv_obj_move_foreground(cap);
        else                lv_obj_move_foreground(g_lock_icon);
    }
    if (g_aod_root  != nullptr)   lv_obj_move_foreground(g_aod_root);
    if (g_menu_root != nullptr)   lv_obj_move_foreground(g_menu_root);
    lvgl_port_unlock();
}
