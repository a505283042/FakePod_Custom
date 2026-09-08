#include "cassette_view.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "png.h"

#include "audio_service.h"
#include "board_pins.h"
#include "cover_surface_cache.h"
#include "fallback_cover_images.h"
#include "font/font_manager.h"
#include "media_catalog_v2.h"
#include "media_library.h"
#include "player_state.h"
#include "system/screen_lock_simple.h"
#include "lyrics/lyrics_service.h"
#include "gesture/gesture_router.h"
#include "ui_common.h"

static const char *TAG = "磁带视觉";

// Music 磁带视觉：CoverSurface + 静态外壳 + 机械件 Sprite + C2.4三段走带。音频链保持只读。
static constexpr int16_t kCassetteWidth = 460;
static constexpr int16_t kCassetteHeight = 296;
static constexpr int16_t kCassetteX = 0;
// C1.3：磁带从正中央上移，为顶部曲目信息和底部 Mini Lyrics 留空间。
static constexpr int16_t kCassetteY = 68;

static constexpr int16_t kInfoX = 20;
static constexpr int16_t kInfoWidth = 420;
static constexpr int16_t kTitleY = 5;
static constexpr int16_t kTitleHeight = 29;
static constexpr int16_t kArtistY = 35;
static constexpr int16_t kArtistHeight = 24;
static constexpr int16_t kCurrentLyricY = 374;
static constexpr int16_t kCurrentLyricHeight = 38;
static constexpr int16_t kNextLyricY = 415;
static constexpr int16_t kNextLyricHeight = 30;

// 对用户最终 460x296 Overlay 做 Alpha 连通域扫描后，主 Label 透明开窗精确为
// x=30, y=25, 400x186。封面按 410px 宽等比缩小，左右各保留约 5px 出血，
// 再由 400px viewport 裁切，避免透明边缘露缝，同时比原 460px 1:1 裁切看到更多封面内容。
static constexpr int16_t kLabelX = 30;
static constexpr int16_t kLabelY = 25;
static constexpr int16_t kLabelWidth = 400;
static constexpr int16_t kLabelHeight = 186;
static constexpr uint32_t kCoverBleedWidth = 410U;
static constexpr uint32_t kLvImageScaleNone = 256U;

// C2.4.9：封面纵向微调。步进收细到4px；边界不再只靠固定±100px，
// 而是根据“当前封面实际缩放高度 - Label开窗高度”动态计算安全范围，
// 并额外保留2px安全余量，保证上下移动都不会露出黑边。
static constexpr int16_t kCoverYOffsetStepPx = 4;
static constexpr int16_t kCoverYOffsetLimitPx = 100;
static constexpr int16_t kCoverEdgeSafetyPx = 2;
static constexpr int16_t kCoverAdjustButtonSize = 40;
// C2.4.13：视觉仍保持40x40，但点击热区向四周扩20px（约80x80），进一步提升命中率。
static constexpr int16_t kCoverAdjustExtraClickPx = 20;
// C2.4.11：在C2.4.9基础上再下移10px，更贴合横向粉色壳体区域。
static constexpr int16_t kCoverAdjustButtonY = 222;
static constexpr int16_t kCoverAdjustUpButtonX = 97;
static constexpr int16_t kCoverAdjustDownButtonX = 323;

// C2.3：用户提供的机械件严格按最终 460x296 外壳像素坐标放置。
// 大轮 56x56 使用 12 帧覆盖六齿结构的一个 60°视觉周期（每帧5°）；
// 8 点小轮 44x44 继续只保留 2 个唯一相位（0/22.5°）。
// 运行期仅移动 Sprite Strip，不做 LVGL rotate/scale。
static constexpr uint8_t kBigReelFrameCount = 12U;
static constexpr uint8_t kSmallRollerFrameCount = 2U;
static constexpr int16_t kBigReelSize = 56;
static constexpr int16_t kBigReelLeftX = 106;
static constexpr int16_t kBigReelRightX = 297;
static constexpr int16_t kBigReelY = 106;
static constexpr int16_t kSmallRollerSize = 44;
static constexpr int16_t kSmallRollerLeftX = 27;
static constexpr int16_t kSmallRollerRightX = 389;
static constexpr int16_t kSmallRollerY = 244;
static constexpr int16_t kTapeAmountWidth = 159;
static constexpr int16_t kTapeAmountHeight = 41;
static constexpr int16_t kTapeAmountBaseX = 150;
static constexpr int16_t kTapeAmountY = 104;
static constexpr int16_t kTapeAmountTravelPx = 25;
static constexpr int64_t kMechanicsFramePeriodUs = 50000LL;  // C2.3：20 Hz，12帧大轮真正显示5°中间相位
static constexpr uint32_t kMechanicsTimerPeriodMs = 50U;
// C2.4.15：96/192 kHz FLAC 给解码/I2S 留更大的实时预算：机械 timer 仍保持 50ms，
// 但大轮只允许每 100ms 提交一次新画面，并只使用 12 帧条带中的 6 个 10°相位。
// 这样不需要切换/重解码 Sprite 资源，也能把高采样率播放时的机械 UI 压力减半。
static constexpr int64_t kHighRateFlacMechanicsFramePeriodUs = 100000LL;
static constexpr uint8_t kHighRateFlacBigReelFrameCount = 6U;
static_assert(kBigReelFrameCount % kHighRateFlacBigReelFrameCount == 0U);

// C2.4.1：走带仍采用3段简化路径，但全部锚在圆周而不是圆心。
// 中间横线贴两个小轮开窗的下圆周；左右斜线只显示在 Label 下方的粉色外壳区域，
// 其“虚拟”大轮端固定从大轮外侧圆周切点起算，并随磁带量最多向外移动5px。
// 不做实时切线/三角函数，只用固定锚点 + 线性插值求 Label 下边缘的可见起点。
static constexpr uint32_t kTapeSideColorHex = 0x83515E;
static constexpr uint32_t kTapeMiddleColorHex = 0x965B6A;
static constexpr uint32_t kTapeGlintColorHex = 0xFFD0DC;
static constexpr uint8_t kTapeSideOpa = 218U;
static constexpr uint8_t kTapeMiddleOpa = 232U;
static constexpr uint8_t kTapeLineWidth = 2U;
static constexpr int64_t kTapeGlintPeriodUs = 100000LL;  // 10 Hz，仅几个像素跳动
static constexpr uint8_t kTapeGlintPhaseCount = 4U;
static constexpr uint8_t kCassetteTintHueBins = 18U;
static constexpr uint8_t kCassetteTintStrength = 200U;  // R17：约78%，动态亮色保持鲜明但不荧光
static constexpr uint8_t kCassetteTintMinSaturation = 96U;
static constexpr uint8_t kCassetteTintMaxSaturation = 190U;
static constexpr uint8_t kCassetteTintMinValue = 200U;
static constexpr uint8_t kCassetteTintMaxValue = 224U;

// C2.4.1 固定几何（均为 460x296 磁带局部坐标）。
// 大轮：左轮用左侧圆周切点，右轮用右侧圆周切点；磁带量增加时只沿X向外最多5px。
static constexpr int16_t kTapeBigLeftBaseX = 107;
static constexpr int16_t kTapeBigRightBaseX = 350;
static constexpr int16_t kTapeBigAnchorY = 134;
static constexpr int16_t kTapeBigOuterTravelPx = 5;
// C2.4.4：斜线接小轮的位置按最终外壳开孔固定在“外侧上圆周”。
// 左轮取约10~11点方向，右轮取约1~2点方向；C2.4.6 再各收回1px，取消额外外扩，
// 让2px走带线端点落在粉色开孔圆周边缘。
// 坐标均为460x296磁带局部坐标，不做运行时切线/三角函数。
static constexpr int16_t kTapeSmallLeftSideX = 35;
static constexpr int16_t kTapeSmallRightSideX = 424;
static constexpr int16_t kTapeSmallSideY = 250;
// Label透明开窗 y=25..210，211开始进入粉色外壳；斜线只从这里开始显示。
static constexpr int16_t kTapeShellVisibleTopY = kLabelY + kLabelHeight;
// 中间线以两个小轮透明开窗的下圆周为基准，C2.4.2 整体再下移2px。
static constexpr int16_t kTapeSmallLeftBottomX = 48;
static constexpr int16_t kTapeSmallRightBottomX = 411;
static constexpr int16_t kTapeSmallBottomY = 285;

// 大卷轴采用 Q16.16“帧相位”累积。12帧后每帧相差5°；把刷新周期从100ms
// 缩短到50ms，并保持 0.5~1.0 frame/tick，因此角速度仍约为50~100°/s，
// 与C2.2一致，但视觉步进从最多10°降到最多5°。
static constexpr uint32_t kPhaseOneFrameQ16 = 1U << 16U;
static constexpr uint32_t kBigReelSlowStepQ16 = kPhaseOneFrameQ16 / 2U;
static constexpr uint32_t kBigReelFastStepQ16 = kPhaseOneFrameQ16;
// 小滚轮仍保持C2.2约每100ms切换一次相位；20Hz timer下每tick推进半帧。
static constexpr uint32_t kSmallRollerStepQ16 = kPhaseOneFrameQ16 / 2U;

extern "C" {
extern const uint8_t g_cassette_shell_png[];
extern const size_t g_cassette_shell_png_size;
extern const uint8_t g_cassette_big_reel_strip_png[];
extern const size_t g_cassette_big_reel_strip_png_size;
extern const uint8_t g_cassette_small_roller_strip_png[];
extern const size_t g_cassette_small_roller_strip_png_size;
extern const uint8_t g_cassette_tape_amount_png[];
extern const size_t g_cassette_tape_amount_png_size;
}

static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_label_viewport = nullptr;
static lv_obj_t *g_cover_image = nullptr;
static lv_obj_t *g_cover_adjust_buttons[2] = {};
static lv_obj_t *g_shell_image = nullptr;
static lv_obj_t *g_tape_amount_image = nullptr;
static lv_obj_t *g_big_reel_viewports[2] = {};
static lv_obj_t *g_big_reel_strip_images[2] = {};
static lv_obj_t *g_small_roller_viewports[2] = {};
static lv_obj_t *g_small_roller_strip_images[2] = {};
static lv_obj_t *g_tape_side_lines[2] = {};
static lv_obj_t *g_tape_middle_line = nullptr;
static lv_obj_t *g_tape_glints[5] = {};
static lv_point_precise_t g_tape_side_points[2][2] = {};
static lv_point_precise_t g_tape_middle_points[2] = {};
static lv_obj_t *g_title_label = nullptr;
static lv_obj_t *g_artist_label = nullptr;
static lv_obj_t *g_current_lyric_label = nullptr;
static lv_obj_t *g_next_lyric_label = nullptr;
static bool g_active = false;
static bool g_controls_visible = false;
// C2.4.10：进度条拖动和Audio Seek提交期间冻结所有磁带机械刷新。
static bool g_seek_frozen = false;
// C2.4.13：Launcher 显示期间冻结 Cassette 机械层。高速 Launcher 会进一步隐藏整个
// Cassette root；fallback 则保留静态磁带背景，避免菜单切换时大小轮继续重绘。
static bool g_launcher_suspended = false;

static uint32_t g_text_track = UINT32_MAX;
static uint32_t g_lyrics_requested_track = UINT32_MAX;
static uint32_t g_last_lyrics_revision = 0U;
static uint32_t g_last_lyrics_line = UINT32_MAX;

static uint8_t *g_shell_pixels = nullptr;
static lv_image_dsc_t g_shell_dsc = {};
static uint8_t *g_shell_base_rgb565 = nullptr;
static uint8_t *g_shell_tint_luma = nullptr;
static uint32_t g_shell_tintable_pixels = 0U;
static uint32_t g_shell_tint_generation = 0U;
static uint32_t g_shell_tint_track = UINT32_MAX;

struct CassetteTintColor
{
    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;
};

// R17：不再固定落在少数色板中。封面主色保留自己的 Hue，
// 只把饱和度和明度收敛到明亮、干净、接近原装粉色塑料质感的范围。
static constexpr CassetteTintColor kCassetteNeutralTint = {122U, 128U, 136U};

static uint8_t *g_big_reel_pixels = nullptr;
static uint8_t *g_small_roller_pixels = nullptr;
static uint8_t *g_small_roller_base_rgb565 = nullptr;
static uint8_t *g_small_roller_tint_luma = nullptr;
static uint32_t g_small_roller_tintable_pixels = 0U;
static uint8_t *g_tape_amount_pixels = nullptr;
static lv_image_dsc_t g_big_reel_dsc = {};
static lv_image_dsc_t g_small_roller_dsc = {};
static lv_image_dsc_t g_tape_amount_dsc = {};
static bool g_mechanics_ready = false;
static uint32_t g_big_reel_phase_q16[2] = {};
static uint32_t g_small_roller_phase_q16 = 0U;
static int16_t g_last_tape_shift = INT16_MIN;
static int8_t g_last_tape_path_step = -1;
static int64_t g_last_mechanics_frame_us = 0LL;
static lv_timer_t *g_mechanics_timer = nullptr;
static uint8_t g_tape_glint_phase = 0U;
static int64_t g_last_tape_glint_us = 0LL;

static CoverSurfaceLease g_cover_lease = {};
static FallbackCoverImageLease g_fallback_cover_lease = {};
static lv_image_dsc_t g_cover_dsc = {};
static uint32_t g_cover_generation = 0U;
static uint32_t g_cover_track = UINT32_MAX;
static uint32_t g_cover_scale_q8 = kLvImageScaleNone;
static int16_t g_cover_y_offset_px = 0;
static bool g_cover_is_no_artwork_fallback = false;

static void cassette_view_init_rgb565_dsc(
    lv_image_dsc_t *dsc,
    const uint8_t *data,
    uint16_t width,
    uint16_t height,
    size_t size);
static void cassette_view_restore_shell_default();
static void cassette_view_restore_small_roller_default();
static bool cassette_view_prepare_small_roller_tint_assets();

static uint16_t cassette_view_cover_source_width()
{
    return g_cover_is_no_artwork_fallback
        ? g_fallback_cover_lease.width
        : g_cover_lease.width;
}

static uint16_t cassette_view_cover_source_height()
{
    return g_cover_is_no_artwork_fallback
        ? g_fallback_cover_lease.height
        : g_cover_lease.height;
}

static int16_t cassette_view_cover_safe_offset_limit_px()
{
    const uint16_t source_height = cassette_view_cover_source_height();
    if (source_height == 0U || g_cover_scale_q8 == 0U) return 0;

    // 保守地按向下取整后的实际缩放高度计算，避免LVGL整数缩放边缘出现1px黑缝。
    const int32_t scaled_height = static_cast<int32_t>(
        (static_cast<uint64_t>(source_height) * g_cover_scale_q8) / kLvImageScaleNone);
    const int32_t extra_height = scaled_height - static_cast<int32_t>(kLabelHeight);
    if (extra_height <= 2 * kCoverEdgeSafetyPx) return 0;

    int32_t safe = extra_height / 2 - kCoverEdgeSafetyPx;
    if (safe < 0) safe = 0;
    if (safe > kCoverYOffsetLimitPx) safe = kCoverYOffsetLimitPx;
    return static_cast<int16_t>(safe);
}

static void cassette_view_apply_cover_position()
{
    const uint16_t source_width = cassette_view_cover_source_width();
    const uint16_t source_height = cassette_view_cover_source_height();
    if (g_cover_image == nullptr || source_width == 0U || source_height == 0U) return;

    const int16_t safe_limit = cassette_view_cover_safe_offset_limit_px();
    if (g_cover_y_offset_px < -safe_limit) g_cover_y_offset_px = -safe_limit;
    if (g_cover_y_offset_px > safe_limit) g_cover_y_offset_px = safe_limit;

    // 明确使用“未缩放图像居中基准 + Y偏移”。LVGL缩放默认绕图像中心进行，
    // 因此正Y必然向下、负Y必然向上，不再依赖center()后的相对坐标状态。
    const int32_t base_x =
        (static_cast<int32_t>(kLabelWidth) - static_cast<int32_t>(source_width)) / 2;
    const int32_t base_y =
        (static_cast<int32_t>(kLabelHeight) - static_cast<int32_t>(source_height)) / 2;
    lv_obj_set_pos(
        g_cover_image,
        static_cast<int16_t>(base_x),
        static_cast<int16_t>(base_y + g_cover_y_offset_px));
}

static void cassette_view_cover_adjust_capture_cb(lv_event_t *event)
{
    if (event == nullptr) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        // 箭头是磁带自身控件：接管本次按压，避免 GestureRouter 把它解释成页面滑动。
        gesture_router_set_control_capture(true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        gesture_router_set_control_capture(false);
    }
}

static void cassette_view_cover_adjust_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED || !g_active) return;

    // C2.4.9：不再通过user_data里的±1判断方向，直接按实际按钮对象区分，
    // 避免两个按钮因为方向数据异常而发生同向移动。
    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    int16_t delta = 0;
    const char *action = nullptr;
    if (target == g_cover_adjust_buttons[0]) {
        delta = -kCoverYOffsetStepPx;  // 左▲：图像真正向上移动
        action = "上移";
    } else if (target == g_cover_adjust_buttons[1]) {
        delta = +kCoverYOffsetStepPx;  // 右▼：图像真正向下移动
        action = "下移";
    } else {
        return;
    }

    const int16_t safe_limit = cassette_view_cover_safe_offset_limit_px();
    int32_t limited = static_cast<int32_t>(g_cover_y_offset_px) + delta;
    if (limited < -safe_limit) limited = -safe_limit;
    if (limited > safe_limit) limited = safe_limit;
    if (limited == g_cover_y_offset_px) {
        ESP_LOGI(TAG, "磁带封面%s已到安全边界：%dpx", action, static_cast<int>(g_cover_y_offset_px));
        return;
    }

    g_cover_y_offset_px = static_cast<int16_t>(limited);
    cassette_view_apply_cover_position();
    ESP_LOGI(TAG, "磁带封面%s：%dpx（安全范围±%dpx）",
        action,
        static_cast<int>(g_cover_y_offset_px),
        static_cast<int>(safe_limit));
}

static lv_obj_t *cassette_view_create_cover_adjust_button(
    lv_obj_t *parent, int16_t local_x, const char *symbol)
{
    lv_obj_t *button = lv_obj_create(parent);
    if (button == nullptr) return nullptr;
    ui_common_lock_object(button);
    lv_obj_set_pos(button, kCassetteX + local_x, kCassetteY + kCoverAdjustButtonY);
    lv_obj_set_size(button, kCoverAdjustButtonSize, kCoverAdjustButtonSize);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFD6E2), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, 38, LV_STATE_PRESSED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(button, kCoverAdjustExtraClickPx);
    // 明确禁止CLICKED向screen冒泡：点箭头只调封面，不触发主页“单击显示控件”。
    lv_obj_remove_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *label = lv_label_create(button);
    if (label == nullptr) return nullptr;
    ui_common_lock_object(label);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, lv_font_default(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFF0F5), 0);
    lv_obj_set_style_text_opa(label, 205, 0);
    lv_obj_center(label);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_add_event_cb(button, cassette_view_cover_adjust_capture_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(
        button, cassette_view_cover_adjust_cb, LV_EVENT_CLICKED, nullptr);
    return button;
}


static lv_obj_t *cassette_view_create_text_label(
    lv_obj_t *parent,
    int16_t y,
    int16_t height,
    lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == nullptr) return nullptr;
    ui_common_lock_object(label);
    lv_obj_set_pos(label, kInfoX, y);
    lv_obj_set_size(label, kInfoWidth, height);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(label, 0, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font_manager_get_ui_font(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, "");
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

static void cassette_view_set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label == nullptr) return;
    const char *safe = text != nullptr ? text : "";
    const char *current = lv_label_get_text(label);
    if (current == nullptr || strcmp(current, safe) != 0) {
        lv_label_set_text(label, safe);
    }
}

static void cassette_view_apply_aux_visibility()
{
    const bool visible = g_active && !g_controls_visible;
    lv_obj_t *labels[] = {g_title_label, g_artist_label, g_current_lyric_label, g_next_lyric_label};
    for (lv_obj_t *label : labels) {
        if (label == nullptr) continue;
        if (visible) {
            lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void cassette_view_clear_lyrics()
{
    cassette_view_set_text_if_changed(g_current_lyric_label, "");
    cassette_view_set_text_if_changed(g_next_lyric_label, "");
    g_last_lyrics_revision = 0U;
    g_last_lyrics_line = UINT32_MAX;
}

static void cassette_view_update_track_text()
{
    if (!player_state_is_ready() || media_library_get_count() == 0U) {
        g_text_track = UINT32_MAX;
        cassette_view_set_text_if_changed(g_title_label, "暂无歌曲");
        cassette_view_set_text_if_changed(g_artist_label, "");
        cassette_view_clear_lyrics();
        return;
    }

    const uint32_t track = static_cast<uint32_t>(player_state_get_index());
    if (track == g_text_track) return;

    MediaTrackViewV2 view = {};
    const bool have_view = media_catalog_v2_get_track_view(track, &view);
    char title_fallback[512] = {};
    const char *title = nullptr;
    if (have_view && view.title != nullptr && view.title[0] != '\0') {
        title = view.title;
    } else if (media_library_copy_display_name(track, title_fallback, sizeof(title_fallback))) {
        title = title_fallback;
    } else {
        snprintf(title_fallback, sizeof(title_fallback), "歌曲 %lu",
            static_cast<unsigned long>(track + 1U));
        title = title_fallback;
    }
    cassette_view_set_text_if_changed(g_title_label, title);

    const char *artist = have_view && view.artist != nullptr && view.artist[0] != '\0'
        ? view.artist
        : "未知歌手";
    cassette_view_set_text_if_changed(g_artist_label, artist);

    g_text_track = track;
    g_lyrics_requested_track = UINT32_MAX;
    cassette_view_clear_lyrics();
}

static void cassette_view_update_mini_lyrics()
{
    if (g_controls_visible || !lyrics_service_is_ready() ||
        !player_state_is_ready() || media_library_get_count() == 0U) {
        return;
    }

    const uint32_t track = static_cast<uint32_t>(player_state_get_index());
    AudioStateSnapshot audio = {};
    const bool audio_ok = audio_service_get_snapshot(&audio);
    const uint64_t position_ms =
        audio_ok && audio.ready && audio.track_index == track ? audio.position_ms : 0U;

    LyricsWindowSnapshot window = {};
    if (!lyrics_service_get_window(track, position_ms, &window)) {
        return;
    }

    if (window.track_index == track && window.state != LyricsLoadState::Idle) {
        g_lyrics_requested_track = track;
    } else if (g_lyrics_requested_track != track && lyrics_service_request_track(track)) {
        g_lyrics_requested_track = track;
    }

    if (window.track_index != track || window.state != LyricsLoadState::Ready) {
        if (g_last_lyrics_revision != window.revision || g_last_lyrics_line != UINT32_MAX) {
            cassette_view_clear_lyrics();
            g_last_lyrics_revision = window.revision;
        }
        return;
    }

    if (window.revision == g_last_lyrics_revision &&
        window.current_line_index == g_last_lyrics_line) {
        return;
    }

    g_last_lyrics_revision = window.revision;
    g_last_lyrics_line = window.current_line_index;

    const LyricsWindowLine &current = window.lines[2];
    const LyricsWindowLine &next = window.lines[3];
    cassette_view_set_text_if_changed(
        g_current_lyric_label,
        current.valid && current.current ? current.text : "");
    cassette_view_set_text_if_changed(
        g_next_lyric_label,
        next.valid ? next.text : "");
}

static bool cassette_view_bind_no_artwork_label(uint32_t generation, uint32_t track)
{
    if (g_cover_image == nullptr) return false;

    CoverSurfaceLease old_cover = g_cover_lease;
    FallbackCoverImageLease old_fallback = g_fallback_cover_lease;
    g_cover_lease = {};
    g_fallback_cover_lease = {};
    g_cover_dsc = {};
    g_cover_scale_q8 = kLvImageScaleNone;
    if (g_cover_track != track) {
        g_cover_y_offset_px = 0;
    }
    g_cover_generation = generation;
    g_cover_track = track;
    g_cover_is_no_artwork_fallback = true;
    // 真正无封面时保持产品默认粉色，不让替补标签纸参与壳体/小轮取色。
    cassette_view_restore_shell_default();
    cassette_view_restore_small_roller_default();

    const bool acquired = fallback_cover_image_acquire(
        FallbackCoverImageKind::Cassette, &g_fallback_cover_lease);
    if (acquired && g_fallback_cover_lease.rgb565 != nullptr &&
        g_fallback_cover_lease.width > 0U && g_fallback_cover_lease.height > 0U) {
        cassette_view_init_rgb565_dsc(
            &g_cover_dsc,
            g_fallback_cover_lease.rgb565,
            g_fallback_cover_lease.width,
            g_fallback_cover_lease.height,
            g_fallback_cover_lease.data_size);
        lv_image_set_src(g_cover_image, &g_cover_dsc);

        const uint32_t width_scale = static_cast<uint32_t>(
            (static_cast<uint64_t>(kCoverBleedWidth) * kLvImageScaleNone +
             g_fallback_cover_lease.width - 1U) /
            g_fallback_cover_lease.width);
        const uint32_t min_cover_height =
            static_cast<uint32_t>(kLabelHeight + 2 * kCoverEdgeSafetyPx);
        const uint32_t height_scale = static_cast<uint32_t>(
            (static_cast<uint64_t>(min_cover_height) * kLvImageScaleNone +
             g_fallback_cover_lease.height - 1U) /
            g_fallback_cover_lease.height);
        g_cover_scale_q8 = width_scale > height_scale ? width_scale : height_scale;
        if (g_cover_scale_q8 == 0U) g_cover_scale_q8 = 1U;

        lv_image_set_scale(g_cover_image, g_cover_scale_q8);
        lv_image_set_antialias(g_cover_image, false);
        cassette_view_apply_cover_position();
        lv_obj_remove_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG,
            "磁带标签使用TF替补封面：track=%lu path=/sdcard/System/no_cover_cassette.jpg scale=%u/256",
            static_cast<unsigned long>(track),
            static_cast<unsigned>(g_cover_scale_q8));
    } else {
        // 文件缺失/尺寸错误时只保留黑色Label底，不退回代码绘制替补。
        lv_obj_add_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGW(TAG,
            "磁带标签替补JPG不可用：track=%lu；请放置460x460 /sdcard/System/no_cover_cassette.jpg",
            static_cast<unsigned long>(track));
    }

    if (old_cover.slot_index != 0xFFU) {
        cover_surface_cache_release(&old_cover);
    }
    if (old_fallback.slot_index != 0xFFU) {
        fallback_cover_image_release(&old_fallback);
    }
    fallback_cover_image_discard_unpinned();
    return true;
}

static void cassette_view_release_cover()
{
    if (g_cover_lease.slot_index != 0xFFU) {
        cover_surface_cache_release(&g_cover_lease);
    }
    if (g_fallback_cover_lease.slot_index != 0xFFU) {
        fallback_cover_image_release(&g_fallback_cover_lease);
    }
    g_cover_lease = {};
    g_fallback_cover_lease = {};
    g_cover_dsc = {};
    g_cover_generation = 0U;
    g_cover_track = UINT32_MAX;
    g_cover_scale_q8 = kLvImageScaleNone;
    g_cover_is_no_artwork_fallback = false;
    if (g_cover_image != nullptr) {
        lv_obj_add_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);
    }
    fallback_cover_image_discard_unpinned();
}

static void cassette_view_init_rgb565_dsc(
    lv_image_dsc_t *dsc,
    const uint8_t *data,
    uint16_t width,
    uint16_t height,
    size_t size)
{
    if (dsc == nullptr) return;
    *dsc = {};
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.flags = 0U;
    dsc->header.w = width;
    dsc->header.h = height;
    dsc->header.stride = static_cast<uint32_t>(width) * 2U;
    dsc->data_size = static_cast<uint32_t>(size);
    dsc->data = data;
}

static bool cassette_view_decode_png_rgb565a8(
    const uint8_t *png_data,
    size_t png_size,
    uint16_t expected_width,
    uint16_t expected_height,
    const char *asset_name,
    uint8_t **out_pixels,
    lv_image_dsc_t *out_dsc)
{
    if (png_data == nullptr || png_size == 0U || out_pixels == nullptr || out_dsc == nullptr) {
        return false;
    }

    png_image image = {};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, png_data, png_size)) {
        ESP_LOGE(TAG, "%s PNG header解析失败", asset_name != nullptr ? asset_name : "机械件");
        return false;
    }
    if (image.width != expected_width || image.height != expected_height) {
        ESP_LOGE(TAG, "%s尺寸错误：%lux%lu expected=%ux%u",
            asset_name != nullptr ? asset_name : "机械件",
            static_cast<unsigned long>(image.width),
            static_cast<unsigned long>(image.height),
            static_cast<unsigned>(expected_width),
            static_cast<unsigned>(expected_height));
        png_image_free(&image);
        return false;
    }

    image.format = PNG_FORMAT_RGBA;
    const size_t rgba_bytes = PNG_IMAGE_SIZE(image);
    uint8_t *rgba = static_cast<uint8_t *>(heap_caps_malloc(
        rgba_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (rgba == nullptr) {
        ESP_LOGE(TAG, "%s临时RGBA PSRAM不足：%uB",
            asset_name != nullptr ? asset_name : "机械件",
            static_cast<unsigned>(rgba_bytes));
        png_image_free(&image);
        return false;
    }

    if (!png_image_finish_read(&image, nullptr, rgba, 0, nullptr)) {
        ESP_LOGE(TAG, "%s PNG解码失败", asset_name != nullptr ? asset_name : "机械件");
        heap_caps_free(rgba);
        png_image_free(&image);
        return false;
    }
    png_image_free(&image);

    const size_t pixel_count = static_cast<size_t>(expected_width) * expected_height;
    const size_t rgb_bytes = pixel_count * 2U;
    const size_t native_bytes = rgb_bytes + pixel_count;
    uint8_t *native = static_cast<uint8_t *>(heap_caps_malloc(
        native_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (native == nullptr) {
        ESP_LOGE(TAG, "%s RGB565A8 PSRAM不足：%uB",
            asset_name != nullptr ? asset_name : "机械件",
            static_cast<unsigned>(native_bytes));
        heap_caps_free(rgba);
        return false;
    }

    for (size_t index = 0U; index < pixel_count; ++index) {
        const uint8_t r = rgba[index * 4U + 0U];
        const uint8_t g = rgba[index * 4U + 1U];
        const uint8_t b = rgba[index * 4U + 2U];
        const uint8_t a = rgba[index * 4U + 3U];
        const uint16_t rgb565 = static_cast<uint16_t>(
            ((static_cast<uint16_t>(r) & 0xF8U) << 8U) |
            ((static_cast<uint16_t>(g) & 0xFCU) << 3U) |
            (static_cast<uint16_t>(b) >> 3U));
        native[index * 2U + 0U] = static_cast<uint8_t>(rgb565 & 0xFFU);
        native[index * 2U + 1U] = static_cast<uint8_t>(rgb565 >> 8U);
        native[rgb_bytes + index] = a;
    }
    heap_caps_free(rgba);

    *out_dsc = {};
    out_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    out_dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    out_dsc->header.flags = 0U;
    out_dsc->header.w = expected_width;
    out_dsc->header.h = expected_height;
    out_dsc->header.stride = static_cast<uint32_t>(expected_width) * 2U;
    out_dsc->data_size = static_cast<uint32_t>(native_bytes);
    out_dsc->data = native;
    *out_pixels = native;
    return true;
}

static void cassette_view_apply_mechanics_frames(
    uint8_t left_big_frame,
    uint8_t right_big_frame,
    uint8_t small_frame)
{
    if (!g_mechanics_ready) return;

    const uint8_t big_frames[2] = {
        static_cast<uint8_t>(left_big_frame % kBigReelFrameCount),
        static_cast<uint8_t>(right_big_frame % kBigReelFrameCount),
    };
    for (size_t i = 0U; i < 2U; ++i) {
        if (g_big_reel_strip_images[i] != nullptr) {
            lv_obj_set_x(g_big_reel_strip_images[i],
                -static_cast<int32_t>(big_frames[i]) * kBigReelSize);
        }
    }

    // 8 点小轮的视觉周期为 45°，0° / 22.5° 两帧已经覆盖全部唯一相位。
    // 因为只有两个唯一相位，正/反方向在视觉上等价，两侧直接共用同一帧。
    small_frame %= kSmallRollerFrameCount;
    for (size_t i = 0U; i < 2U; ++i) {
        if (g_small_roller_strip_images[i] != nullptr) {
            lv_obj_set_x(g_small_roller_strip_images[i],
                -static_cast<int32_t>(small_frame) * kSmallRollerSize);
        }
    }
}

static lv_obj_t *cassette_view_create_tape_line(
    lv_obj_t *parent,
    lv_point_precise_t *points,
    uint32_t point_count,
    uint32_t color_hex,
    uint8_t opacity)
{
    lv_obj_t *line = lv_line_create(parent);
    if (line == nullptr) return nullptr;
    ui_common_lock_object(line);
    lv_line_set_points(line, points, point_count);
    lv_obj_set_style_line_width(line, kTapeLineWidth, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color_hex), 0);
    lv_obj_set_style_line_opa(line, opacity, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    return line;
}

static lv_obj_t *cassette_view_create_tape_glint(
    lv_obj_t *parent,
    int16_t width,
    int16_t height)
{
    lv_obj_t *dot = lv_obj_create(parent);
    if (dot == nullptr) return nullptr;
    ui_common_lock_object(dot);
    lv_obj_set_size(dot, width, height);
    lv_obj_set_style_radius(dot, 1, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(kTapeGlintColorHex), 0);
    lv_obj_set_style_bg_opa(dot, 0, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_shadow_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    return dot;
}

static lv_point_precise_t cassette_view_interpolate_point(
    const lv_point_precise_t &a,
    const lv_point_precise_t &b,
    int32_t numerator,
    int32_t denominator)
{
    lv_point_precise_t out = a;
    if (denominator <= 0) return out;
    out.x = static_cast<int32_t>(a.x) +
        ((static_cast<int32_t>(b.x) - static_cast<int32_t>(a.x)) * numerator) / denominator;
    out.y = static_cast<int32_t>(a.y) +
        ((static_cast<int32_t>(b.y) - static_cast<int32_t>(a.y)) * numerator) / denominator;
    return out;
}

static void cassette_view_apply_tape_glints(bool seeking)
{
    const uint8_t phase = static_cast<uint8_t>(g_tape_glint_phase % kTapeGlintPhaseCount);
    static constexpr int8_t kTravel[4] = {0, 1, 3, 2};
    static constexpr int8_t kJitterY[4] = {0, -1, 0, 1};
    static constexpr uint8_t kOpa[4] = {125U, 205U, 165U, 105U};

    const int16_t travel = static_cast<int16_t>(kTravel[phase]) * (seeking ? 2 : 1);
    const int16_t jitter_y = static_cast<int16_t>(kJitterY[phase]) * (seeking ? 2 : 1);
    const uint8_t opacity = static_cast<uint8_t>(
        kOpa[phase] + (seeking && kOpa[phase] <= 225U ? 20U : 0U));

    // 左右斜线高光严格沿当前可见线段移动；大轮端随进度变化后，高光会自动跟随新几何。
    for (size_t i = 0U; i < 2U; ++i) {
        lv_obj_t *dot = g_tape_glints[i];
        if (dot == nullptr) continue;
        // 以线段中部为基准，每个相位只前后滑动几个像素，不做整条发光。
        const int32_t t = 16 + static_cast<int32_t>(kTravel[phase]) * (seeking ? 2 : 1);
        lv_point_precise_t p = cassette_view_interpolate_point(
            g_tape_side_points[i][0], g_tape_side_points[i][1], t, 32);
        lv_obj_set_pos(dot, static_cast<int16_t>(p.x), static_cast<int16_t>(p.y + jitter_y));
        lv_obj_set_style_bg_opa(dot, opacity, 0);
    }

    // 中间横线只放3个短亮点，不整条发光；整体跟随下移后的固定走带 y=285。
    static constexpr int16_t kMiddleBaseX[3] = {148, 229, 313};
    for (size_t i = 0U; i < 3U; ++i) {
        lv_obj_t *dot = g_tape_glints[i + 2U];
        if (dot == nullptr) continue;
        const int16_t stagger = static_cast<int16_t>((i * 2U + phase) % 3U);
        lv_obj_set_pos(dot,
            kCassetteX + kMiddleBaseX[i] + travel + stagger,
            kCassetteY + kTapeSmallBottomY - 1 + jitter_y);
        const uint8_t local_opa = static_cast<uint8_t>(
            opacity > i * 12U ? opacity - i * 12U : 70U);
        lv_obj_set_style_bg_opa(dot, local_opa, 0);
    }
}

static void cassette_view_update_tape_path_geometry(uint32_t progress_q16)
{
    // 0%：左卷少 -> 左大轮端在基础切点；右卷多 -> 右端向外5px。
    // 100%：左卷多 -> 左端向外5px；右卷少 -> 右端回基础切点。
    if (progress_q16 > kPhaseOneFrameQ16) progress_q16 = kPhaseOneFrameQ16;
    const int16_t left_extra = static_cast<int16_t>(
        (static_cast<uint64_t>(kTapeBigOuterTravelPx) * progress_q16) / kPhaseOneFrameQ16);
    const int16_t right_extra = static_cast<int16_t>(
        (static_cast<uint64_t>(kTapeBigOuterTravelPx) *
         (kPhaseOneFrameQ16 - progress_q16)) / kPhaseOneFrameQ16);
    const int8_t step = static_cast<int8_t>((left_extra << 4) | right_extra);
    if (step == g_last_tape_path_step) return;
    g_last_tape_path_step = step;

    const int16_t big_x[2] = {
        static_cast<int16_t>(kTapeBigLeftBaseX - left_extra),
        static_cast<int16_t>(kTapeBigRightBaseX + right_extra),
    };
    const int16_t small_x[2] = {kTapeSmallLeftSideX, kTapeSmallRightSideX};

    for (size_t i = 0U; i < 2U; ++i) {
        // 先用“虚拟完整线”从大轮圆周锚点连接到小轮圆周锚点，
        // 再只取与 Label 下边缘 y=211 的交点作为屏幕可见起点。
        // 因此线不会画在封面上，但大轮端的5px外移仍会自然改变外壳上的斜线方向。
        const int32_t dy = kTapeSmallSideY - kTapeBigAnchorY;
        const int32_t visible_dy = kTapeShellVisibleTopY - kTapeBigAnchorY;
        const int32_t dx = static_cast<int32_t>(small_x[i]) - big_x[i];
        const int16_t visible_x = static_cast<int16_t>(
            static_cast<int32_t>(big_x[i]) + (dx * visible_dy) / dy);

        g_tape_side_points[i][0].x = kCassetteX + visible_x;
        g_tape_side_points[i][0].y = kCassetteY + kTapeShellVisibleTopY;
        g_tape_side_points[i][1].x = kCassetteX + small_x[i];
        g_tape_side_points[i][1].y = kCassetteY + kTapeSmallSideY;
        if (g_tape_side_lines[i] != nullptr) {
            lv_line_set_points(g_tape_side_lines[i], g_tape_side_points[i], 2U);
        }
    }

    cassette_view_apply_tape_glints(false);
}

static void cassette_view_set_tape_path_visible(bool visible)
{
    lv_obj_t *objects[] = {
        g_tape_side_lines[0], g_tape_side_lines[1], g_tape_middle_line,
        g_tape_glints[0], g_tape_glints[1], g_tape_glints[2],
        g_tape_glints[3], g_tape_glints[4],
    };
    for (lv_obj_t *obj : objects) {
        if (obj == nullptr) continue;
        if (visible && g_mechanics_ready) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void cassette_view_set_mechanics_visible(bool visible)
{
    lv_obj_t *objects[] = {
        g_tape_amount_image,
        g_big_reel_viewports[0],
        g_big_reel_viewports[1],
        g_small_roller_viewports[0],
        g_small_roller_viewports[1],
    };
    for (lv_obj_t *obj : objects) {
        if (obj == nullptr) continue;
        if (visible && g_mechanics_ready) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    cassette_view_set_tape_path_visible(visible);
}

static bool cassette_view_prepare_mechanics()
{
    if (g_mechanics_ready) return true;

    if (g_big_reel_pixels == nullptr &&
        !cassette_view_decode_png_rgb565a8(
            g_cassette_big_reel_strip_png,
            g_cassette_big_reel_strip_png_size,
            static_cast<uint16_t>(kBigReelSize * kBigReelFrameCount),
            static_cast<uint16_t>(kBigReelSize),
            "大卷轴Sprite",
            &g_big_reel_pixels,
            &g_big_reel_dsc)) {
        return false;
    }
    if (g_small_roller_pixels == nullptr &&
        !cassette_view_decode_png_rgb565a8(
            g_cassette_small_roller_strip_png,
            g_cassette_small_roller_strip_png_size,
            static_cast<uint16_t>(kSmallRollerSize * kSmallRollerFrameCount),
            static_cast<uint16_t>(kSmallRollerSize),
            "小滚轮Sprite",
            &g_small_roller_pixels,
            &g_small_roller_dsc)) {
        return false;
    }
    if (!cassette_view_prepare_small_roller_tint_assets()) {
        ESP_LOGW(TAG, "小滚轮着色缓存准备失败：保留原粉色，不影响机械动画");
    }
    if (g_tape_amount_pixels == nullptr &&
        !cassette_view_decode_png_rgb565a8(
            g_cassette_tape_amount_png,
            g_cassette_tape_amount_png_size,
            static_cast<uint16_t>(kTapeAmountWidth),
            static_cast<uint16_t>(kTapeAmountHeight),
            "磁带量",
            &g_tape_amount_pixels,
            &g_tape_amount_dsc)) {
        return false;
    }

    for (size_t i = 0U; i < 2U; ++i) {
        if (g_big_reel_strip_images[i] != nullptr) {
            lv_image_set_src(g_big_reel_strip_images[i], &g_big_reel_dsc);
            lv_image_set_antialias(g_big_reel_strip_images[i], false);
        }
        if (g_small_roller_strip_images[i] != nullptr) {
            lv_image_set_src(g_small_roller_strip_images[i], &g_small_roller_dsc);
            lv_image_set_antialias(g_small_roller_strip_images[i], false);
        }
    }
    if (g_tape_amount_image != nullptr) {
        lv_image_set_src(g_tape_amount_image, &g_tape_amount_dsc);
        lv_image_set_antialias(g_tape_amount_image, false);
    }

    g_mechanics_ready = true;
    g_big_reel_phase_q16[0] = 0U;
    g_big_reel_phase_q16[1] = 0U;
    g_small_roller_phase_q16 = 0U;
    g_last_tape_shift = INT16_MIN;
    g_last_tape_path_step = -1;
    g_tape_glint_phase = 0U;
    g_last_mechanics_frame_us = esp_timer_get_time();
    g_last_tape_glint_us = g_last_mechanics_frame_us;
    cassette_view_apply_mechanics_frames(0U, 0U, 0U);
    cassette_view_apply_tape_glints(false);

    const size_t mechanics_bytes =
        static_cast<size_t>(g_big_reel_dsc.data_size) +
        static_cast<size_t>(g_small_roller_dsc.data_size) +
        static_cast<size_t>(g_tape_amount_dsc.data_size);
    ESP_LOGI(TAG,
        "机械件已准备：大轮=%uB(56x56x12) 小轮=%uB(44x44x2) 磁带量=%uB(159x41) total=%uB PSRAM",
        static_cast<unsigned>(g_big_reel_dsc.data_size),
        static_cast<unsigned>(g_small_roller_dsc.data_size),
        static_cast<unsigned>(g_tape_amount_dsc.data_size),
        static_cast<unsigned>(mechanics_bytes));
    ESP_LOGI(TAG, "走带线已准备：3段/2px 粉棕主线 + 5个像素高光，10Hz跳动");
    return true;
}

static uint64_t cassette_view_audio_total_ms(const AudioStateSnapshot &audio)
{
    if (audio.sample_rate_hz == 0U || audio.total_frames == 0U) return 0U;
    return (audio.total_frames * 1000ULL) / audio.sample_rate_hz;
}

static void cassette_view_update_tape_amount(const AudioStateSnapshot &audio)
{
    if (!g_mechanics_ready || g_tape_amount_image == nullptr) return;

    // 换曲时 player_state 会先切到新曲，而 audio snapshot 可能仍短暂保留旧曲。
    // 过渡态直接显示新磁带的起始量，避免先回中间再跳到左边最少。
    int16_t shift = -kTapeAmountTravelPx;
    const uint64_t total_ms = cassette_view_audio_total_ms(audio);
    const bool same_track =
        player_state_is_ready() &&
        audio.track_index == static_cast<uint32_t>(player_state_get_index());
    if (audio.ready && same_track && total_ms > 0U) {
        // C2 实机确认磁带量方向反了。C2.1 完全反转平移方向：
        // 进度0%：图向左25px；100%：图向右25px。
        const uint64_t position_ms = audio.position_ms < total_ms ? audio.position_ms : total_ms;
        const uint64_t travel = static_cast<uint64_t>(kTapeAmountTravelPx) * 2ULL;
        const uint64_t moved = (position_ms * travel) / total_ms;
        shift = static_cast<int16_t>(
            -kTapeAmountTravelPx + static_cast<int32_t>(moved));
    }

    if (shift == g_last_tape_shift) return;
    g_last_tape_shift = shift;
    lv_obj_set_pos(
        g_tape_amount_image,
        kCassetteX + kTapeAmountBaseX + shift,
        kCassetteY + kTapeAmountY);
}

static uint32_t cassette_view_progress_q16(const AudioStateSnapshot &audio)
{
    const uint64_t total_ms = cassette_view_audio_total_ms(audio);
    const bool same_track =
        player_state_is_ready() &&
        audio.track_index == static_cast<uint32_t>(player_state_get_index());
    // 新曲音频快照尚未 ready / 尚未切到当前 track 时，按 0% 起始状态显示。
    // 不再使用 50% 中位兜底，避免切歌瞬间走带几何先回中间。
    if (!audio.ready || !same_track || total_ms == 0U) return 0U;

    const uint64_t position_ms = audio.position_ms < total_ms ? audio.position_ms : total_ms;
    return static_cast<uint32_t>(
        (position_ms * static_cast<uint64_t>(kPhaseOneFrameQ16)) / total_ms);
}

static uint32_t cassette_view_advance_phase_q16(
    uint32_t phase_q16,
    uint32_t frames_per_tick_q16,
    int64_t elapsed_us,
    uint8_t frame_count,
    bool seeking)
{
    if (elapsed_us <= 0LL || frame_count == 0U) return phase_q16;

    // 最多按1.2秒推进。前台长阻塞回来后直接追到新相位，不补画所有中间帧。
    if (elapsed_us > 1200000LL) elapsed_us = 1200000LL;
    uint64_t increment =
        (static_cast<uint64_t>(frames_per_tick_q16) * static_cast<uint64_t>(elapsed_us)) /
        static_cast<uint64_t>(kMechanicsFramePeriodUs);
    if (seeking) increment *= 2ULL;

    const uint32_t cycle_q16 = static_cast<uint32_t>(frame_count) * kPhaseOneFrameQ16;
    return static_cast<uint32_t>((static_cast<uint64_t>(phase_q16) + increment) % cycle_q16);
}

static void cassette_view_update_mechanics()
{
    // C2.4.12：播放控件可见时保持磁带机械画面为最后一帧，避免20Hz机械刷新
    // 与全屏半透明Backdrop/控件刷新叠加。Seek冻结优先级相同，二者任一成立都不刷新。
    if (!g_mechanics_ready || !g_active || g_seek_frozen || g_controls_visible || g_launcher_suspended) return;

    AudioStateSnapshot audio = {};
    if (!audio_service_get_snapshot(&audio)) return;
    const bool high_rate_flac =
        audio.format == MediaFormat::FLAC && audio.sample_rate_hz >= 96000U;
    cassette_view_update_tape_amount(audio);
    const uint32_t progress_q16 = cassette_view_progress_q16(audio);
    cassette_view_update_tape_path_geometry(progress_q16);

    const bool moving =
        audio.state == AudioPlaybackState::Playing ||
        audio.state == AudioPlaybackState::Seeking;
    const int64_t now_us = esp_timer_get_time();
    if (!moving) {
        // Pause/Stopped 时机械运动与亮点跳动都冻结；恢复后不补跑暂停期间的相位。
        g_last_mechanics_frame_us = now_us;
        g_last_tape_glint_us = now_us;
        return;
    }

    if (g_last_mechanics_frame_us <= 0LL) {
        g_last_mechanics_frame_us = now_us;
        return;
    }
    int64_t elapsed_us = now_us - g_last_mechanics_frame_us;
    const int64_t mechanics_frame_period_us = high_rate_flac
        ? kHighRateFlacMechanicsFramePeriodUs : kMechanicsFramePeriodUs;
    if (elapsed_us < mechanics_frame_period_us) return;

    // C2.2：实机确认左右卷轴快慢映射与当前磁带量视觉相反。
    // 按当前磁带量方向修正为：
    // 0%  左边磁带少 -> 左快、右边磁带多 -> 右慢；
    // 100% 左边磁带多 -> 左慢、右边磁带少 -> 右快。
    // 速度仍按进度连续插值，两个大卷轴会平滑交换快慢。
    const uint32_t speed_span_q16 = kBigReelFastStepQ16 - kBigReelSlowStepQ16;
    const uint32_t speed_offset_q16 = static_cast<uint32_t>(
        (static_cast<uint64_t>(speed_span_q16) * progress_q16) / kPhaseOneFrameQ16);
    const uint32_t left_speed_q16 = kBigReelFastStepQ16 - speed_offset_q16;
    const uint32_t right_speed_q16 = kBigReelSlowStepQ16 + speed_offset_q16;
    const bool seeking = audio.state == AudioPlaybackState::Seeking;

    if (g_last_tape_glint_us <= 0LL) g_last_tape_glint_us = now_us;
    const int64_t glint_elapsed_us = now_us - g_last_tape_glint_us;
    if (glint_elapsed_us >= kTapeGlintPeriodUs) {
        uint32_t steps = static_cast<uint32_t>(glint_elapsed_us / kTapeGlintPeriodUs);
        if (steps > kTapeGlintPhaseCount) steps = kTapeGlintPhaseCount;
        if (seeking) steps *= 2U;
        g_tape_glint_phase = static_cast<uint8_t>(
            (g_tape_glint_phase + steps) % kTapeGlintPhaseCount);
        cassette_view_apply_tape_glints(seeking);
        g_last_tape_glint_us = now_us;
    }

    g_big_reel_phase_q16[0] = cassette_view_advance_phase_q16(
        g_big_reel_phase_q16[0], left_speed_q16, elapsed_us, kBigReelFrameCount, seeking);
    g_big_reel_phase_q16[1] = cassette_view_advance_phase_q16(
        g_big_reel_phase_q16[1], right_speed_q16, elapsed_us, kBigReelFrameCount, seeking);

    // 小滚轮保持固定线速度；2帧以100ms节拍交替即可。
    g_small_roller_phase_q16 = cassette_view_advance_phase_q16(
        g_small_roller_phase_q16, kSmallRollerStepQ16, elapsed_us,
        kSmallRollerFrameCount, seeking);

    uint8_t left_big_frame = static_cast<uint8_t>(
        (g_big_reel_phase_q16[0] >> 16U) % kBigReelFrameCount);
    uint8_t right_big_frame = static_cast<uint8_t>(
        (g_big_reel_phase_q16[1] >> 16U) % kBigReelFrameCount);
    if (high_rate_flac) {
        // 12帧条带为每5°一帧；高采样率 FLAC 只取偶数帧，得到原 6 帧的
        // 0/10/20/30/40/50° 相位。相位累积仍连续，切回普通采样率不会跳速度。
        constexpr uint8_t kHighRateFrameStride =
            kBigReelFrameCount / kHighRateFlacBigReelFrameCount;
        left_big_frame = static_cast<uint8_t>(
            (left_big_frame / kHighRateFrameStride) * kHighRateFrameStride);
        right_big_frame = static_cast<uint8_t>(
            (right_big_frame / kHighRateFrameStride) * kHighRateFrameStride);
    }

    cassette_view_apply_mechanics_frames(
        left_big_frame,
        right_big_frame,
        static_cast<uint8_t>((g_small_roller_phase_q16 >> 16U) % kSmallRollerFrameCount));
    g_last_mechanics_frame_us = now_us;
}

static void cassette_view_mechanics_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    // 屏幕动作菜单保持半透明，因此底层机械件必须停在最后一帧。
    if (screen_action_menu_is_open()) {
        return;
    }
    cassette_view_update_mechanics();
}

static lv_obj_t *cassette_view_create_sprite_viewport(
    lv_obj_t *parent,
    int16_t x,
    int16_t y,
    int16_t size,
    lv_obj_t **out_strip_image)
{
    if (out_strip_image == nullptr) return nullptr;
    *out_strip_image = nullptr;

    lv_obj_t *viewport = lv_obj_create(parent);
    if (viewport == nullptr) return nullptr;
    ui_common_lock_object(viewport);
    lv_obj_set_pos(viewport, x, y);
    lv_obj_set_size(viewport, size, size);
    lv_obj_set_style_radius(viewport, 0, 0);
    lv_obj_set_style_bg_opa(viewport, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(viewport, 0, 0);
    lv_obj_set_style_shadow_width(viewport, 0, 0);
    lv_obj_set_style_pad_all(viewport, 0, 0);
    lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(viewport, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(viewport, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(viewport, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *strip = lv_image_create(viewport);
    if (strip == nullptr) return nullptr;
    ui_common_lock_object(strip);
    lv_obj_set_pos(strip, 0, 0);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    *out_strip_image = strip;
    return viewport;
}

static uint8_t cassette_view_clamp_u8(int32_t value)
{
    if (value < 0) return 0U;
    if (value > 255) return 255U;
    return static_cast<uint8_t>(value);
}

static uint16_t cassette_view_load_rgb565(const uint8_t *data)
{
    return static_cast<uint16_t>(
        static_cast<uint16_t>(data[0]) |
        (static_cast<uint16_t>(data[1]) << 8U));
}

static void cassette_view_store_rgb565(uint8_t *data, uint16_t rgb565)
{
    data[0] = static_cast<uint8_t>(rgb565 & 0xFFU);
    data[1] = static_cast<uint8_t>(rgb565 >> 8U);
}

static CassetteTintColor cassette_view_rgb565_to_rgb888(uint16_t rgb565)
{
    CassetteTintColor color = {};
    const uint8_t r5 = static_cast<uint8_t>((rgb565 >> 11U) & 0x1FU);
    const uint8_t g6 = static_cast<uint8_t>((rgb565 >> 5U) & 0x3FU);
    const uint8_t b5 = static_cast<uint8_t>(rgb565 & 0x1FU);
    color.r = static_cast<uint8_t>((static_cast<uint16_t>(r5) * 255U + 15U) / 31U);
    color.g = static_cast<uint8_t>((static_cast<uint16_t>(g6) * 255U + 31U) / 63U);
    color.b = static_cast<uint8_t>((static_cast<uint16_t>(b5) * 255U + 15U) / 31U);
    return color;
}

static uint16_t cassette_view_rgb888_to_rgb565(const CassetteTintColor &color)
{
    return static_cast<uint16_t>(
        ((static_cast<uint16_t>(color.r) & 0xF8U) << 8U) |
        ((static_cast<uint16_t>(color.g) & 0xFCU) << 3U) |
        (static_cast<uint16_t>(color.b) >> 3U));
}

static void cassette_view_rgb_to_hsv(
    const CassetteTintColor &color, uint16_t *out_hue, uint8_t *out_saturation, uint8_t *out_value)
{
    const uint8_t max_value = color.r > color.g
        ? (color.r > color.b ? color.r : color.b)
        : (color.g > color.b ? color.g : color.b);
    const uint8_t min_value = color.r < color.g
        ? (color.r < color.b ? color.r : color.b)
        : (color.g < color.b ? color.g : color.b);
    const int32_t delta = static_cast<int32_t>(max_value) - min_value;

    uint16_t hue = 0U;
    if (delta > 0) {
        int32_t hue_signed = 0;
        if (max_value == color.r) {
            hue_signed = 60 * (static_cast<int32_t>(color.g) - color.b) / delta;
        } else if (max_value == color.g) {
            hue_signed = 120 + 60 * (static_cast<int32_t>(color.b) - color.r) / delta;
        } else {
            hue_signed = 240 + 60 * (static_cast<int32_t>(color.r) - color.g) / delta;
        }
        while (hue_signed < 0) hue_signed += 360;
        while (hue_signed >= 360) hue_signed -= 360;
        hue = static_cast<uint16_t>(hue_signed);
    }

    const uint8_t saturation = max_value == 0U
        ? 0U
        : static_cast<uint8_t>((static_cast<uint32_t>(delta) * 255U + max_value / 2U) / max_value);
    if (out_hue != nullptr) *out_hue = hue;
    if (out_saturation != nullptr) *out_saturation = saturation;
    if (out_value != nullptr) *out_value = max_value;
}

static CassetteTintColor cassette_view_hsv_to_rgb(
    uint16_t hue, uint8_t saturation, uint8_t value)
{
    hue = static_cast<uint16_t>(hue % 360U);
    if (saturation == 0U) return {value, value, value};

    const uint32_t region = hue / 60U;
    const uint32_t remainder = ((hue % 60U) * 255U) / 60U;
    const uint32_t p = (static_cast<uint32_t>(value) * (255U - saturation) + 127U) / 255U;
    const uint32_t q = (static_cast<uint32_t>(value) *
        (255U - (static_cast<uint32_t>(saturation) * remainder + 127U) / 255U) + 127U) / 255U;
    const uint32_t t = (static_cast<uint32_t>(value) *
        (255U - (static_cast<uint32_t>(saturation) * (255U - remainder) + 127U) / 255U) + 127U) / 255U;

    switch (region) {
        case 0U: return {value, static_cast<uint8_t>(t), static_cast<uint8_t>(p)};
        case 1U: return {static_cast<uint8_t>(q), value, static_cast<uint8_t>(p)};
        case 2U: return {static_cast<uint8_t>(p), value, static_cast<uint8_t>(t)};
        case 3U: return {static_cast<uint8_t>(p), static_cast<uint8_t>(q), value};
        case 4U: return {static_cast<uint8_t>(t), static_cast<uint8_t>(p), value};
        default: return {value, static_cast<uint8_t>(p), static_cast<uint8_t>(q)};
    }
}

static CassetteTintColor cassette_view_normalize_dynamic_tint(
    const CassetteTintColor &input, uint16_t *out_hue)
{
    uint16_t hue = 0U;
    uint8_t saturation = 0U;
    uint8_t value = 0U;
    cassette_view_rgb_to_hsv(input, &hue, &saturation, &value);

    // 黄绿色最容易出现“脏黄/荧光绿”。只轻推离 60° 中心，不做色板量化，
    // 仍然保留连续 Hue 和丰富颜色。
    if (hue >= 48U && hue < 60U) {
        hue = static_cast<uint16_t>(hue > 10U ? hue - 10U : 0U);
    } else if (hue >= 60U && hue <= 76U) {
        hue = static_cast<uint16_t>(hue + 12U);
    }

    if (saturation < kCassetteTintMinSaturation) saturation = kCassetteTintMinSaturation;
    if (saturation > kCassetteTintMaxSaturation) saturation = kCassetteTintMaxSaturation;
    if (value < kCassetteTintMinValue) value = kCassetteTintMinValue;
    if (value > kCassetteTintMaxValue) value = kCassetteTintMaxValue;

    // 对偏棕/橙的低彩主色略补饱和，避免提亮后仍显灰。
    if (hue >= 15U && hue <= 45U && saturation < 126U) saturation = 126U;

    if (out_hue != nullptr) *out_hue = hue;
    return cassette_view_hsv_to_rgb(hue, saturation, value);
}

static bool cassette_view_extract_cover_tint(
    const CoverSurfaceLease &cover,
    CassetteTintColor *out_color,
    uint16_t *out_hue,
    uint32_t *out_samples)
{
    if (out_color == nullptr || cover.normal_rgb565 == nullptr ||
        cover.width == 0U || cover.height == 0U || cover.data_size < 2U) {
        return false;
    }

    uint32_t weights[kCassetteTintHueBins] = {};
    uint32_t red_sum[kCassetteTintHueBins] = {};
    uint32_t green_sum[kCassetteTintHueBins] = {};
    uint32_t blue_sum[kCassetteTintHueBins] = {};
    uint32_t accepted = 0U;
    uint32_t sampled = 0U;
    uint32_t near_black_neutral = 0U;

    const uint16_t x_begin = static_cast<uint16_t>(cover.width / 10U);
    const uint16_t x_end = static_cast<uint16_t>(cover.width - cover.width / 10U);
    const uint16_t y_begin = static_cast<uint16_t>(cover.height / 10U);
    const uint16_t y_end = static_cast<uint16_t>(cover.height - cover.height / 10U);
    const uint16_t step_x = cover.width >= 24U ? static_cast<uint16_t>(cover.width / 24U) : 1U;
    const uint16_t step_y = cover.height >= 24U ? static_cast<uint16_t>(cover.height / 24U) : 1U;

    for (uint16_t y = y_begin; y < y_end; y = static_cast<uint16_t>(y + step_y)) {
        for (uint16_t x = x_begin; x < x_end; x = static_cast<uint16_t>(x + step_x)) {
            const size_t pixel_index = static_cast<size_t>(y) * cover.width + x;
            const size_t byte_index = pixel_index * 2U;
            if (byte_index + 1U >= cover.data_size) continue;

            const CassetteTintColor color = cassette_view_rgb565_to_rgb888(
                cassette_view_load_rgb565(cover.normal_rgb565 + byte_index));
            uint16_t hue = 0U;
            uint8_t saturation = 0U;
            uint8_t value = 0U;
            cassette_view_rgb_to_hsv(color, &hue, &saturation, &value);
            ++sampled;
            if (value < 64U && saturation < 96U) ++near_black_neutral;

            // 去掉近黑、近灰和接近白色的背景点，避免白边/黑底把主色拉灰。
            if (value < 44U || saturation < 46U || (value > 244U && saturation < 90U)) {
                continue;
            }

            const uint8_t bin = static_cast<uint8_t>(
                (static_cast<uint32_t>(hue) * kCassetteTintHueBins) / 360U);
            const uint32_t weight = 1U +
                (static_cast<uint32_t>(saturation) * (128U + value)) / 384U;
            weights[bin] += weight;
            red_sum[bin] += static_cast<uint32_t>(color.r) * weight;
            green_sum[bin] += static_cast<uint32_t>(color.g) * weight;
            blue_sum[bin] += static_cast<uint32_t>(color.b) * weight;
            ++accepted;
        }
    }

    if (out_samples != nullptr) *out_samples = accepted;
    // 大面积近黑/中性封面直接使用石墨灰壳体，避免少量彩色文字/Logo抢走主色。
    if (sampled >= 24U && near_black_neutral * 100U >= sampled * 55U) return false;
    if (accepted < 12U) return false;

    uint8_t best_bin = 0U;
    uint32_t best_score = 0U;
    for (uint8_t bin = 0U; bin < kCassetteTintHueBins; ++bin) {
        const uint8_t prev = bin == 0U ? kCassetteTintHueBins - 1U : bin - 1U;
        const uint8_t next = static_cast<uint8_t>((bin + 1U) % kCassetteTintHueBins);
        const uint32_t score = weights[bin] + (weights[prev] + weights[next]) / 2U;
        if (score > best_score) {
            best_score = score;
            best_bin = bin;
        }
    }
    if (best_score == 0U) return false;

    const uint8_t prev = best_bin == 0U ? kCassetteTintHueBins - 1U : best_bin - 1U;
    const uint8_t next = static_cast<uint8_t>((best_bin + 1U) % kCassetteTintHueBins);
    const uint32_t total_weight =
        weights[best_bin] + weights[prev] / 2U + weights[next] / 2U;
    if (total_weight == 0U) return false;

    CassetteTintColor dominant = {};
    dominant.r = static_cast<uint8_t>((
        red_sum[best_bin] + red_sum[prev] / 2U + red_sum[next] / 2U) / total_weight);
    dominant.g = static_cast<uint8_t>((
        green_sum[best_bin] + green_sum[prev] / 2U + green_sum[next] / 2U) / total_weight);
    dominant.b = static_cast<uint8_t>((
        blue_sum[best_bin] + blue_sum[prev] / 2U + blue_sum[next] / 2U) / total_weight);

    *out_color = dominant;
    if (out_hue != nullptr) {
        cassette_view_rgb_to_hsv(*out_color, out_hue, nullptr, nullptr);
    }
    return true;
}

static bool cassette_view_shell_pixel_is_tintable(const CassetteTintColor &color, uint8_t alpha)
{
    if (alpha < 24U) return false;
    const uint16_t luma = static_cast<uint16_t>(
        (77U * color.r + 150U * color.g + 29U * color.b) >> 8U);
    if (luma < 52U) return false;

    // 原素材的塑料壳主体是粉/玫红色：R明显高于G，同时B也高于G。
    // 白色高光、灰色金属、黑色文字/螺丝和棕色磁带细节自然落在Mask之外。
    return color.r > static_cast<uint16_t>(color.g) + 12U &&
        color.b > static_cast<uint16_t>(color.g) + 4U &&
        static_cast<uint16_t>(color.r) + color.b >
            static_cast<uint16_t>(color.g) * 2U + 32U;
}

static bool cassette_view_prepare_small_roller_tint_assets()
{
    if (g_small_roller_base_rgb565 != nullptr && g_small_roller_tint_luma != nullptr) {
        return true;
    }
    if (g_small_roller_pixels == nullptr || g_small_roller_dsc.data == nullptr) return false;

    const size_t width = static_cast<size_t>(kSmallRollerSize) * kSmallRollerFrameCount;
    const size_t height = static_cast<size_t>(kSmallRollerSize);
    const size_t pixel_count = width * height;
    const size_t rgb_bytes = pixel_count * 2U;
    if (g_small_roller_dsc.data_size < rgb_bytes + pixel_count) return false;

    uint8_t *base_rgb565 = static_cast<uint8_t *>(heap_caps_malloc(
        rgb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (base_rgb565 == nullptr) return false;
    uint8_t *tint_luma = static_cast<uint8_t *>(heap_caps_malloc(
        pixel_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (tint_luma == nullptr) {
        heap_caps_free(base_rgb565);
        return false;
    }

    memcpy(base_rgb565, g_small_roller_pixels, rgb_bytes);
    const uint8_t *alpha_plane = g_small_roller_pixels + rgb_bytes;
    uint32_t tintable_pixels = 0U;
    for (size_t index = 0U; index < pixel_count; ++index) {
        const CassetteTintColor source = cassette_view_rgb565_to_rgb888(
            cassette_view_load_rgb565(base_rgb565 + index * 2U));
        if (cassette_view_shell_pixel_is_tintable(source, alpha_plane[index])) {
            const uint8_t luma = static_cast<uint8_t>(
                (77U * source.r + 150U * source.g + 29U * source.b) >> 8U);
            tint_luma[index] = luma >= 254U ? 255U : static_cast<uint8_t>(luma + 1U);
            ++tintable_pixels;
        } else {
            tint_luma[index] = 0U;
        }
    }

    g_small_roller_base_rgb565 = base_rgb565;
    g_small_roller_tint_luma = tint_luma;
    g_small_roller_tintable_pixels = tintable_pixels;
    ESP_LOGI(TAG, "小滚轮着色缓存已准备：Mask=%uB 原色=%uB tintable=%lu PSRAM",
        static_cast<unsigned>(pixel_count),
        static_cast<unsigned>(rgb_bytes),
        static_cast<unsigned long>(g_small_roller_tintable_pixels));
    return true;
}

static void cassette_view_restore_small_roller_default()
{
    if (g_small_roller_pixels == nullptr || g_small_roller_base_rgb565 == nullptr) return;
    const size_t pixel_count =
        static_cast<size_t>(kSmallRollerSize) * kSmallRollerFrameCount * kSmallRollerSize;
    memcpy(g_small_roller_pixels, g_small_roller_base_rgb565, pixel_count * 2U);
    for (lv_obj_t *image : g_small_roller_strip_images) {
        if (image != nullptr) lv_obj_invalidate(image);
    }
}

static void cassette_view_apply_small_roller_tint(const uint16_t tint_lut[256])
{
    if (tint_lut == nullptr || g_small_roller_pixels == nullptr ||
        g_small_roller_base_rgb565 == nullptr || g_small_roller_tint_luma == nullptr) {
        return;
    }

    const size_t pixel_count =
        static_cast<size_t>(kSmallRollerSize) * kSmallRollerFrameCount * kSmallRollerSize;
    for (size_t index = 0U; index < pixel_count; ++index) {
        const uint8_t mapped_luma = g_small_roller_tint_luma[index];
        uint8_t *display_pixel = g_small_roller_pixels + index * 2U;
        if (mapped_luma == 0U) {
            cassette_view_store_rgb565(
                display_pixel, cassette_view_load_rgb565(g_small_roller_base_rgb565 + index * 2U));
        } else {
            cassette_view_store_rgb565(display_pixel, tint_lut[mapped_luma - 1U]);
        }
    }
    for (lv_obj_t *image : g_small_roller_strip_images) {
        if (image != nullptr) lv_obj_invalidate(image);
    }
}

static void cassette_view_build_tint_lut(
    const CassetteTintColor &target, uint16_t out_lut[256])
{
    const int32_t target_luma = static_cast<int32_t>(
        (77U * target.r + 150U * target.g + 29U * target.b) >> 8U);
    const int32_t delta_r = static_cast<int32_t>(target.r) - target_luma;
    const int32_t delta_g = static_cast<int32_t>(target.g) - target_luma;
    const int32_t delta_b = static_cast<int32_t>(target.b) - target_luma;

    for (uint16_t luma = 0U; luma < 256U; ++luma) {
        const uint16_t distance = luma <= 127U ? luma : static_cast<uint16_t>(255U - luma);
        const uint16_t midtone = static_cast<uint16_t>(distance * 2U);
        const int32_t strength = static_cast<int32_t>(
            (static_cast<uint32_t>(kCassetteTintStrength) * midtone) / 255U);

        // R17：继续沿用原壳体像素的 Luma 作为基准，只替换色彩偏移。
        // 阴影、高光和塑料反光因此保持接近原装粉色素材的明暗质感。
        const int32_t base_luma = static_cast<int32_t>(luma);
        CassetteTintColor color = {};
        color.r = cassette_view_clamp_u8(base_luma + delta_r * strength / 255);
        color.g = cassette_view_clamp_u8(base_luma + delta_g * strength / 255);
        color.b = cassette_view_clamp_u8(base_luma + delta_b * strength / 255);
        out_lut[luma] = cassette_view_rgb888_to_rgb565(color);
    }
}

static void cassette_view_restore_shell_default()
{
    if (g_shell_pixels == nullptr || g_shell_base_rgb565 == nullptr) return;
    const size_t pixel_count = static_cast<size_t>(kCassetteWidth) * kCassetteHeight;
    const size_t rgb_bytes = pixel_count * 2U;
    memcpy(g_shell_pixels, g_shell_base_rgb565, rgb_bytes);
    g_shell_tint_generation = 0U;
    g_shell_tint_track = UINT32_MAX;
    if (g_shell_image != nullptr) lv_obj_invalidate(g_shell_image);
}

static bool cassette_view_apply_shell_tint(
    uint32_t generation, uint32_t track, const CoverSurfaceLease &cover)
{
    if (g_shell_pixels == nullptr || g_shell_base_rgb565 == nullptr ||
        g_shell_tint_luma == nullptr) return false;
    if (g_shell_tint_generation == generation && g_shell_tint_track == track) return true;
    const int64_t tint_start_us = esp_timer_get_time();

    CassetteTintColor extracted = {};
    CassetteTintColor target = {};
    uint16_t source_hue = 0U;
    uint16_t target_hue = 0U;
    uint32_t samples = 0U;
    const bool chromatic = cassette_view_extract_cover_tint(
        cover, &extracted, &source_hue, &samples);
    const char *tint_mode = "中性灰";
    if (chromatic) {
        target = cassette_view_normalize_dynamic_tint(extracted, &target_hue);
        tint_mode = "动态亮色";
    } else {
        target = kCassetteNeutralTint;
    }

    uint16_t tint_lut[256] = {};
    cassette_view_build_tint_lut(target, tint_lut);

    const size_t pixel_count = static_cast<size_t>(kCassetteWidth) * kCassetteHeight;
    for (size_t index = 0U; index < pixel_count; ++index) {
        const uint8_t *base_pixel = g_shell_base_rgb565 + index * 2U;
        uint8_t *display_pixel = g_shell_pixels + index * 2U;
        const uint8_t mapped_luma = g_shell_tint_luma[index];
        if (mapped_luma == 0U) {
            cassette_view_store_rgb565(display_pixel, cassette_view_load_rgb565(base_pixel));
            continue;
        }
        cassette_view_store_rgb565(display_pixel, tint_lut[mapped_luma - 1U]);
    }

    cassette_view_apply_small_roller_tint(tint_lut);

    g_shell_tint_generation = generation;
    g_shell_tint_track = track;
    if (g_shell_image != nullptr) lv_obj_invalidate(g_shell_image);
    const int64_t tint_cost_us = esp_timer_get_time() - tint_start_us;
    ESP_LOGI(TAG,
        "磁带壳/小轮动态亮色：track=%lu mode=%s samples=%lu source_hue=%u target_hue=%u rgb=#%02X%02X%02X shell=%lu roller=%lu cost=%lldus",
        static_cast<unsigned long>(track),
        tint_mode,
        static_cast<unsigned long>(samples),
        static_cast<unsigned>(source_hue),
        static_cast<unsigned>(target_hue),
        static_cast<unsigned>(target.r),
        static_cast<unsigned>(target.g),
        static_cast<unsigned>(target.b),
        static_cast<unsigned long>(g_shell_tintable_pixels),
        static_cast<unsigned long>(g_small_roller_tintable_pixels),
        static_cast<long long>(tint_cost_us));
    return true;
}

static bool cassette_view_prepare_shell()
{
    if (g_shell_pixels != nullptr) {
        return true;
    }

    const size_t png_size = g_cassette_shell_png_size;
    if (png_size == 0U) {
        ESP_LOGE(TAG, "磁带壳 PNG 资源为空");
        return false;
    }

    png_image image = {};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, g_cassette_shell_png, png_size)) {
        ESP_LOGE(TAG, "磁带壳 PNG header 解析失败");
        return false;
    }
    if (image.width != kCassetteWidth || image.height != kCassetteHeight) {
        ESP_LOGE(TAG, "磁带壳尺寸错误：%lux%lu expected=%dx%d",
            static_cast<unsigned long>(image.width),
            static_cast<unsigned long>(image.height),
            static_cast<int>(kCassetteWidth),
            static_cast<int>(kCassetteHeight));
        png_image_free(&image);
        return false;
    }

    image.format = PNG_FORMAT_RGBA;
    const size_t rgba_bytes = PNG_IMAGE_SIZE(image);
    uint8_t *rgba = static_cast<uint8_t *>(heap_caps_malloc(
        rgba_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (rgba == nullptr) {
        ESP_LOGE(TAG, "磁带壳临时 RGBA PSRAM 不足：%uB", static_cast<unsigned>(rgba_bytes));
        png_image_free(&image);
        return false;
    }

    if (!png_image_finish_read(&image, nullptr, rgba, 0, nullptr)) {
        ESP_LOGE(TAG, "磁带壳 PNG 解码失败");
        heap_caps_free(rgba);
        png_image_free(&image);
        return false;
    }
    png_image_free(&image);

    const size_t pixel_count = static_cast<size_t>(kCassetteWidth) * kCassetteHeight;
    // LVGL 9 RGB565A8 is planar, not interleaved:
    // [RGB565 plane: width*height*2][A8 plane: width*height].
    // header.stride describes the RGB565 plane only (width*2 bytes).
    const size_t rgb_bytes = pixel_count * 2U;
    const size_t alpha_bytes = pixel_count;
    const size_t native_bytes = rgb_bytes + alpha_bytes;
    uint8_t *native = static_cast<uint8_t *>(heap_caps_malloc(
        native_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (native == nullptr) {
        ESP_LOGE(TAG, "磁带壳 RGB565A8 PSRAM 不足：%uB", static_cast<unsigned>(native_bytes));
        heap_caps_free(rgba);
        return false;
    }

    uint8_t *base_rgb565 = static_cast<uint8_t *>(heap_caps_malloc(
        rgb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (base_rgb565 == nullptr) {
        ESP_LOGE(TAG, "磁带壳原色 RGB565 PSRAM 不足：%uB", static_cast<unsigned>(rgb_bytes));
        heap_caps_free(native);
        heap_caps_free(rgba);
        return false;
    }

    uint8_t *tint_luma = static_cast<uint8_t *>(heap_caps_malloc(
        pixel_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (tint_luma == nullptr) {
        ESP_LOGE(TAG, "磁带壳着色Mask PSRAM 不足：%uB", static_cast<unsigned>(pixel_count));
        heap_caps_free(base_rgb565);
        heap_caps_free(native);
        heap_caps_free(rgba);
        return false;
    }

    uint32_t tintable_pixels = 0U;
    for (size_t index = 0U; index < pixel_count; ++index) {
        const uint8_t r = rgba[index * 4U + 0U];
        const uint8_t g = rgba[index * 4U + 1U];
        const uint8_t b = rgba[index * 4U + 2U];
        const uint8_t a = rgba[index * 4U + 3U];
        const uint16_t rgb565 = static_cast<uint16_t>(
            ((static_cast<uint16_t>(r) & 0xF8U) << 8U) |
            ((static_cast<uint16_t>(g) & 0xFCU) << 3U) |
            (static_cast<uint16_t>(b) >> 3U));
        native[index * 2U + 0U] = static_cast<uint8_t>(rgb565 & 0xFFU);
        native[index * 2U + 1U] = static_cast<uint8_t>(rgb565 >> 8U);
        native[rgb_bytes + index] = a;

        const CassetteTintColor source_color = {r, g, b};
        if (cassette_view_shell_pixel_is_tintable(source_color, a)) {
            const uint8_t luma = static_cast<uint8_t>(
                (77U * r + 150U * g + 29U * b) >> 8U);
            tint_luma[index] = luma >= 254U ? 255U : static_cast<uint8_t>(luma + 1U);
            ++tintable_pixels;
        } else {
            tint_luma[index] = 0U;
        }
    }
    memcpy(base_rgb565, native, rgb_bytes);
    heap_caps_free(rgba);

    g_shell_pixels = native;
    g_shell_base_rgb565 = base_rgb565;
    g_shell_tint_luma = tint_luma;
    g_shell_tintable_pixels = tintable_pixels;
    g_shell_tint_generation = 0U;
    g_shell_tint_track = UINT32_MAX;
    g_shell_dsc = {};
    g_shell_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_shell_dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    g_shell_dsc.header.flags = 0U;
    g_shell_dsc.header.w = kCassetteWidth;
    g_shell_dsc.header.h = kCassetteHeight;
    g_shell_dsc.header.stride = static_cast<uint32_t>(kCassetteWidth) * 2U;
    g_shell_dsc.data_size = static_cast<uint32_t>(native_bytes);
    g_shell_dsc.data = g_shell_pixels;

    if (g_shell_image != nullptr) {
        lv_image_set_src(g_shell_image, &g_shell_dsc);
        lv_image_set_antialias(g_shell_image, false);
    }

    ESP_LOGI(TAG, "磁带壳已准备：PNG=%uB RGB565A8=%uB 原色=%uB Mask=%uB tintable=%lu stride=%u PSRAM",
        static_cast<unsigned>(png_size),
        static_cast<unsigned>(native_bytes),
        static_cast<unsigned>(rgb_bytes),
        static_cast<unsigned>(pixel_count),
        static_cast<unsigned long>(g_shell_tintable_pixels),
        static_cast<unsigned>(g_shell_dsc.header.stride));
    return true;
}

static bool cassette_view_bind_current_cover()
{
    if (!player_state_is_ready() || media_library_get_count() == 0U || g_cover_image == nullptr) {
        return false;
    }

    const uint32_t generation = media_catalog_v2_generation();
    const uint32_t track = static_cast<uint32_t>(player_state_get_index());
    if (g_cover_is_no_artwork_fallback &&
        g_cover_generation == generation && g_cover_track == track) {
        return true;
    }
    if (g_cover_lease.slot_index != 0xFFU &&
        g_cover_generation == generation && g_cover_track == track) {
        return true;
    }

    // “没有封面”与“封面仍在后台准备”必须分开处理：
    // 前者立即切到磁带专用标签纸替补；后者继续保留上一张视觉，直到新 Surface ready。
    MediaArtworkViewV2 artwork = {};
    if (!media_library_get_artwork_view(track, &artwork)) {
        return cassette_view_bind_no_artwork_label(generation, track);
    }

    CoverSurfaceLease next = {};
    if (!cover_surface_cache_acquire(track, &next) || next.normal_rgb565 == nullptr ||
        next.width == 0U || next.height == 0U || next.data_size == 0U) {
        return false;
    }

    // 先 acquire 新 Surface，再释放旧 Surface/替补图，保持切歌视觉连续。
    CoverSurfaceLease old = g_cover_lease;
    FallbackCoverImageLease old_fallback = g_fallback_cover_lease;
    g_fallback_cover_lease = {};
    g_cover_lease = next;
    g_cover_is_no_artwork_fallback = false;
    // C2.4.15：封面纵向手动调节只属于当前歌曲，切歌后新封面回到居中。
    if (g_cover_track != track) {
        g_cover_y_offset_px = 0;
    }
    g_cover_generation = generation;
    g_cover_track = track;
    cassette_view_init_rgb565_dsc(
        &g_cover_dsc,
        g_cover_lease.normal_rgb565,
        g_cover_lease.width,
        g_cover_lease.height,
        g_cover_lease.data_size);

    lv_image_set_src(g_cover_image, &g_cover_dsc);
    uint32_t cover_scale = kLvImageScaleNone;
    if (g_cover_lease.width > 0U && g_cover_lease.height > 0U) {
        // 宽度仍以410px出血为目标；同时保证纵向至少覆盖Label并预留安全边缘。
        // 两者取更大的scale，避免非正方形封面纵向不足时移动后露黑。
        const uint32_t width_scale = static_cast<uint32_t>(
            (static_cast<uint64_t>(kCoverBleedWidth) * kLvImageScaleNone +
             static_cast<uint64_t>(g_cover_lease.width) - 1U) /
            static_cast<uint64_t>(g_cover_lease.width));
        const uint32_t min_cover_height =
            static_cast<uint32_t>(kLabelHeight + 2 * kCoverEdgeSafetyPx);
        const uint32_t height_scale = static_cast<uint32_t>(
            (static_cast<uint64_t>(min_cover_height) * kLvImageScaleNone +
             static_cast<uint64_t>(g_cover_lease.height) - 1U) /
            static_cast<uint64_t>(g_cover_lease.height));
        cover_scale = width_scale > height_scale ? width_scale : height_scale;
        if (cover_scale == 0U) cover_scale = 1U;
    }
    g_cover_scale_q8 = cover_scale;
    lv_image_set_scale(g_cover_image, cover_scale);
    lv_image_set_antialias(g_cover_image, false);
    cassette_view_apply_cover_position();
    lv_obj_remove_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);
    // 新封面 Surface 已 ready：只在切歌时分析一次主色并重着色静态壳体。
    (void)cassette_view_apply_shell_tint(generation, track, g_cover_lease);

    ESP_LOGI(TAG, "磁带封面已绑定：track=%lu source=%ux%u label=%dx%d bleed=%upx scale=%u/256 y=%dpx safe=±%dpx",
        static_cast<unsigned long>(track),
        static_cast<unsigned>(g_cover_lease.width),
        static_cast<unsigned>(g_cover_lease.height),
        static_cast<int>(kLabelWidth),
        static_cast<int>(kLabelHeight),
        static_cast<unsigned>(kCoverBleedWidth),
        static_cast<unsigned>(cover_scale),
        static_cast<int>(g_cover_y_offset_px),
        static_cast<int>(cassette_view_cover_safe_offset_limit_px()));

    if (old.slot_index != 0xFFU) {
        cover_surface_cache_release(&old);
    }
    if (old_fallback.slot_index != 0xFFU) {
        fallback_cover_image_release(&old_fallback);
        fallback_cover_image_discard_unpinned();
    }
    return true;
}

esp_err_t cassette_view_create(lv_obj_t *parent)
{
    if (parent == nullptr) return ESP_ERR_INVALID_ARG;
    if (g_root != nullptr) return ESP_OK;

    g_launcher_suspended = false;
    g_root = lv_obj_create(parent);
    if (g_root == nullptr) return ESP_ERR_NO_MEM;
    ui_common_lock_object(g_root);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_size(g_root, FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT);
    lv_obj_set_style_radius(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_shadow_width(g_root, 0, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);

    g_title_label = cassette_view_create_text_label(
        g_root, kTitleY, kTitleHeight, lv_color_hex(0xFFFFFF));
    g_artist_label = cassette_view_create_text_label(
        g_root, kArtistY, kArtistHeight, lv_color_hex(0xAEB6C2));
    g_current_lyric_label = cassette_view_create_text_label(
        g_root, kCurrentLyricY, kCurrentLyricHeight, lv_color_hex(0xFFFFFF));
    g_next_lyric_label = cassette_view_create_text_label(
        g_root, kNextLyricY, kNextLyricHeight, lv_color_hex(0x737D8B));
    if (g_title_label == nullptr || g_artist_label == nullptr ||
        g_current_lyric_label == nullptr || g_next_lyric_label == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    g_label_viewport = lv_obj_create(g_root);
    if (g_label_viewport == nullptr) return ESP_ERR_NO_MEM;
    ui_common_lock_object(g_label_viewport);
    lv_obj_set_pos(g_label_viewport, kCassetteX + kLabelX, kCassetteY + kLabelY);
    lv_obj_set_size(g_label_viewport, kLabelWidth, kLabelHeight);
    lv_obj_set_style_radius(g_label_viewport, 0, 0);
    lv_obj_set_style_bg_color(g_label_viewport, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_label_viewport, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_label_viewport, 0, 0);
    lv_obj_set_style_shadow_width(g_label_viewport, 0, 0);
    lv_obj_set_style_pad_all(g_label_viewport, 0, 0);
    lv_obj_remove_flag(g_label_viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g_label_viewport, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_label_viewport, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // 无封面歌曲的磁带标签直接使用 TF 卡 460x460 JPG：
    // /sdcard/System/no_cover_cassette.jpg。它与真实封面共用下面的 g_cover_image，
    // 由 400x186 Label viewport 裁切，因此不会增加持续 JPG 解码开销。

    g_cover_image = lv_image_create(g_label_viewport);
    if (g_cover_image == nullptr) return ESP_ERR_NO_MEM;
    ui_common_lock_object(g_cover_image);
    lv_obj_add_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);

    // C2：机械件全部位于 CoverSurface 之上、粉色 Shell Overlay 之下。
    // 这样外壳自身会自然裁掉 Sprite 的出血边缘，不需要运行时 Mask。
    g_tape_amount_image = lv_image_create(g_root);
    if (g_tape_amount_image == nullptr) return ESP_ERR_NO_MEM;
    ui_common_lock_object(g_tape_amount_image);
    lv_obj_set_pos(
        g_tape_amount_image,
        // 初始即使用新磁带 0% 状态，避免对象第一次显示时短暂位于中间。
        kCassetteX + kTapeAmountBaseX - kTapeAmountTravelPx,
        kCassetteY + kTapeAmountY);
    lv_obj_add_flag(g_tape_amount_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_tape_amount_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_tape_amount_image, LV_OBJ_FLAG_SCROLLABLE);

    // C2.4.1：左右斜线改到 Shell 之后创建，直接显示在粉色外壳表面；
    // 具体可见端点稍后由固定圆周锚点初始化。

    g_big_reel_viewports[0] = cassette_view_create_sprite_viewport(
        g_root,
        kCassetteX + kBigReelLeftX,
        kCassetteY + kBigReelY,
        kBigReelSize,
        &g_big_reel_strip_images[0]);
    g_big_reel_viewports[1] = cassette_view_create_sprite_viewport(
        g_root,
        kCassetteX + kBigReelRightX,
        kCassetteY + kBigReelY,
        kBigReelSize,
        &g_big_reel_strip_images[1]);
    g_small_roller_viewports[0] = cassette_view_create_sprite_viewport(
        g_root,
        kCassetteX + kSmallRollerLeftX,
        kCassetteY + kSmallRollerY,
        kSmallRollerSize,
        &g_small_roller_strip_images[0]);
    g_small_roller_viewports[1] = cassette_view_create_sprite_viewport(
        g_root,
        kCassetteX + kSmallRollerRightX,
        kCassetteY + kSmallRollerY,
        kSmallRollerSize,
        &g_small_roller_strip_images[1]);
    if (g_big_reel_viewports[0] == nullptr || g_big_reel_viewports[1] == nullptr ||
        g_small_roller_viewports[0] == nullptr || g_small_roller_viewports[1] == nullptr ||
        g_big_reel_strip_images[0] == nullptr || g_big_reel_strip_images[1] == nullptr ||
        g_small_roller_strip_images[0] == nullptr || g_small_roller_strip_images[1] == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    g_shell_image = lv_image_create(g_root);
    if (g_shell_image == nullptr) return ESP_ERR_NO_MEM;
    ui_common_lock_object(g_shell_image);
    lv_obj_set_pos(g_shell_image, kCassetteX, kCassetteY);
    lv_obj_add_flag(g_shell_image, LV_OBJ_FLAG_HIDDEN);

    // C2.4.1：三条走带都位于 Shell 之上，因此只画“应该看得见的外壳表面”部分。
    // 左右斜线不会再穿过封面：可见起点固定在 Label 下边缘，由大轮圆周虚拟锚点决定斜率。
    g_tape_side_points[0][0] = {kCassetteX + 77, kCassetteY + kTapeShellVisibleTopY};
    g_tape_side_points[0][1] = {kCassetteX + kTapeSmallLeftSideX, kCassetteY + kTapeSmallSideY};
    g_tape_side_points[1][0] = {kCassetteX + 382, kCassetteY + kTapeShellVisibleTopY};
    g_tape_side_points[1][1] = {kCassetteX + kTapeSmallRightSideX, kCassetteY + kTapeSmallSideY};
    g_tape_side_lines[0] = cassette_view_create_tape_line(
        g_root, g_tape_side_points[0], 2U, kTapeSideColorHex, kTapeSideOpa);
    g_tape_side_lines[1] = cassette_view_create_tape_line(
        g_root, g_tape_side_points[1], 2U, kTapeSideColorHex, kTapeSideOpa);
    g_tape_glints[0] = cassette_view_create_tape_glint(g_root, 2, 2);
    g_tape_glints[1] = cassette_view_create_tape_glint(g_root, 2, 2);
    if (g_tape_side_lines[0] == nullptr || g_tape_side_lines[1] == nullptr ||
        g_tape_glints[0] == nullptr || g_tape_glints[1] == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // 中间线以两个小轮开窗下圆周为基准，并整体再下移2px，不穿过圆心。
    g_tape_middle_points[0].x = kCassetteX + kTapeSmallLeftBottomX;
    g_tape_middle_points[0].y = kCassetteY + kTapeSmallBottomY;
    g_tape_middle_points[1].x = kCassetteX + kTapeSmallRightBottomX;
    g_tape_middle_points[1].y = kCassetteY + kTapeSmallBottomY;
    g_tape_middle_line = cassette_view_create_tape_line(
        g_root, g_tape_middle_points, 2U, kTapeMiddleColorHex, kTapeMiddleOpa);
    g_tape_glints[2] = cassette_view_create_tape_glint(g_root, 3, 2);
    g_tape_glints[3] = cassette_view_create_tape_glint(g_root, 4, 2);
    g_tape_glints[4] = cassette_view_create_tape_glint(g_root, 2, 2);
    if (g_tape_middle_line == nullptr || g_tape_glints[2] == nullptr ||
        g_tape_glints[3] == nullptr || g_tape_glints[4] == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    g_last_tape_path_step = -1;
    cassette_view_update_tape_path_geometry(0U);
    cassette_view_apply_tape_glints(false);

    // C2.4.13：封面微调箭头放在横向粉色区域左右两侧，并在上一版基础上再下移10px；
    // 视觉仍为40x40，实际点击热区约80x80。
    // 左▲固定上移、右▼固定下移；按钮自身接管触摸，因此不会触发主页播放控件。
    g_cover_adjust_buttons[0] = cassette_view_create_cover_adjust_button(
        g_root, kCoverAdjustUpButtonX, LV_SYMBOL_UP);
    g_cover_adjust_buttons[1] = cassette_view_create_cover_adjust_button(
        g_root, kCoverAdjustDownButtonX, LV_SYMBOL_DOWN);
    if (g_cover_adjust_buttons[0] == nullptr || g_cover_adjust_buttons[1] == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    g_mechanics_timer = lv_timer_create(
        cassette_view_mechanics_timer_cb, kMechanicsTimerPeriodMs, nullptr);
    if (g_mechanics_timer == nullptr) return ESP_ERR_NO_MEM;
    lv_timer_pause(g_mechanics_timer);

    return ESP_OK;
}

bool cassette_view_set_active(bool active)
{
    if (g_root == nullptr) return false;
    if (active) {
        if (!cassette_view_prepare_shell()) {
            return false;
        }
        const bool mechanics_ok = cassette_view_prepare_mechanics();
        if (!mechanics_ok) {
            ESP_LOGW(TAG, "机械件准备失败：降级为静态磁带壳，不影响Music播放");
        }
        if (g_shell_image != nullptr) {
            lv_obj_remove_flag(g_shell_image, LV_OBJ_FLAG_HIDDEN);
        }
        (void)cassette_view_bind_current_cover();
        lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
        g_active = true;
        g_last_mechanics_frame_us = esp_timer_get_time();
        g_last_tape_glint_us = g_last_mechanics_frame_us;
        if (g_mechanics_timer != nullptr) {
            if (mechanics_ok && !g_seek_frozen && !g_controls_visible && !g_launcher_suspended) {
                lv_timer_resume(g_mechanics_timer);
            } else {
                lv_timer_pause(g_mechanics_timer);
            }
        }
        cassette_view_set_mechanics_visible(mechanics_ok);
        cassette_view_update_track_text();
        cassette_view_apply_aux_visibility();
        cassette_view_update_mini_lyrics();
        cassette_view_update_mechanics();
        return true;
    }

    g_active = false;
    if (g_mechanics_timer != nullptr) lv_timer_pause(g_mechanics_timer);
    cassette_view_set_mechanics_visible(false);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    cassette_view_release_cover();
    g_text_track = UINT32_MAX;
    g_lyrics_requested_track = UINT32_MAX;
    cassette_view_clear_lyrics();
    cassette_view_apply_aux_visibility();
    return true;
}

void cassette_view_update()
{
    if (!g_active || g_root == nullptr) return;
    (void)cassette_view_bind_current_cover();
    cassette_view_update_track_text();
    cassette_view_update_mini_lyrics();
    cassette_view_update_mechanics();
}

void cassette_view_set_controls_visible(bool visible)
{
    if (g_controls_visible == visible) {
        cassette_view_apply_aux_visibility();
        return;
    }

    g_controls_visible = visible;
    cassette_view_apply_aux_visibility();
    if (!g_active) {
        // Artwork 模式也会同步 Controls 状态；只记状态，不操作未激活的磁带机械层。
        return;
    }

    // C2.4.12：打开播放控件时保留半透明Backdrop，但冻结所有磁带机械刷新。
    // 只暂停timer并重置时间基准，不隐藏/重建机械对象，因此画面保持最后一帧。
    const int64_t now_us = esp_timer_get_time();
    g_last_mechanics_frame_us = now_us;
    g_last_tape_glint_us = now_us;

    const bool should_resume_mechanics =
        !visible && g_active && g_mechanics_ready && !g_seek_frozen && !g_launcher_suspended;
    if (g_mechanics_timer != nullptr && visible) {
        lv_timer_pause(g_mechanics_timer);
    }

    if (g_active && !visible) {
        // 控件关闭后先按当前真实播放进度同步一次磁带量/走带几何；
        // 因时间基准刚重置，不会补跑控件显示期间漏掉的卷轴帧。
        cassette_view_update();
    }

    if (g_mechanics_timer != nullptr && should_resume_mechanics) {
        lv_timer_reset(g_mechanics_timer);
        lv_timer_resume(g_mechanics_timer);
    }

    ESP_LOGI(TAG, "控件机械动画：%s", visible ? "冻结" : "恢复");
}

void cassette_view_set_launcher_suspended(bool suspended)
{
    if (g_launcher_suspended == suspended) return;
    g_launcher_suspended = suspended;

    // Launcher 切换与 Controls/Seek Freeze 一样：只重置时间基准，不补跑暂停期间的动画。
    const int64_t now_us = esp_timer_get_time();
    g_last_mechanics_frame_us = now_us;
    g_last_tape_glint_us = now_us;

    if (g_mechanics_timer != nullptr) {
        if (suspended) {
            lv_timer_pause(g_mechanics_timer);
        } else if (g_active && g_mechanics_ready && !g_seek_frozen && !g_controls_visible) {
            // Launcher 收起后先按当前真实播放位置同步一次，再恢复机械 timer。
            cassette_view_update_mechanics();
            lv_timer_reset(g_mechanics_timer);
            lv_timer_resume(g_mechanics_timer);
        }
    }

    ESP_LOGI(TAG, "Launcher机械动画：%s", suspended ? "冻结" : "恢复");
}

void cassette_view_set_seek_frozen(bool frozen)
{
    if (g_seek_frozen == frozen) return;
    g_seek_frozen = frozen;
    if (!g_active) {
        // Artwork 模式的进度条同样会同步 Seek 状态；磁带未激活时不操作机械 timer。
        return;
    }

    // 冻结/解冻时都重置时间基准，恢复后不补跑拖动/Seek期间漏掉的动画帧。
    const int64_t now_us = esp_timer_get_time();
    g_last_mechanics_frame_us = now_us;
    g_last_tape_glint_us = now_us;

    if (g_mechanics_timer != nullptr) {
        if (frozen) {
            lv_timer_pause(g_mechanics_timer);
        } else if (g_active && g_mechanics_ready && !g_controls_visible && !g_launcher_suspended) {
            // 先按Seek完成后的真实播放位置一次性同步磁带量/走带几何，再恢复20Hz动画。
            // 若此时播放控件仍可见，则继续保持冻结，等控件关闭后再恢复。
            cassette_view_update_mechanics();
            lv_timer_reset(g_mechanics_timer);
            lv_timer_resume(g_mechanics_timer);
        }
    }

    ESP_LOGI(TAG, "Seek机械动画：%s", frozen ? "冻结" : "恢复");
}

bool cassette_view_is_active()
{
    return g_active;
}
