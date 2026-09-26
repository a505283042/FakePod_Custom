#include "cassette_view.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "png.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "artwork_loader.h"
#include "audio_service.h"
#include "board_pins.h"
#include "cover_surface_cache.h"
#include "fallback_cover_images.h"
#include "font/font_manager.h"
#include "media_catalog_v2.h"
#include "media_library.h"
#include "player_state.h"
#include "player_control.h"
#include "system/device_settings.h"
#include "system/screen_lock_simple.h"
#include "lyrics/lyrics_service.h"
#include "lyrics/lyrics_text_layout.h"
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
static constexpr int16_t kCurrentLyricHeight = 62;
static constexpr int32_t kMiniLyricTextMaxW = 408;
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

// Controls 兜底背景仍由独立低优先级任务直接在 PSRAM 合成，不调用 LVGL Snapshot。
// 正常磁带预取不再制作 460x460 next 控件整屏；下一首只 pin normal CoverSurface，
// 并预做 460x296 变色磁带壳，切歌时直接提升这两份资源。
static constexpr uint8_t kControlsCacheDimOpacity = 150U;
static constexpr uint16_t kControlsCacheWidth = FAKEPOD_LCD_WIDTH;
static constexpr uint16_t kControlsCacheHeight = FAKEPOD_LCD_HEIGHT;
static constexpr size_t kControlsCacheBytes =
    static_cast<size_t>(kControlsCacheWidth) * kControlsCacheHeight * sizeof(uint16_t);
static constexpr size_t kPrefetchShellBytes =
    static_cast<size_t>(kCassetteWidth) * kCassetteHeight * sizeof(uint16_t);
static constexpr uint32_t kCassetteCacheTaskStackBytes = 5120U;
static constexpr UBaseType_t kCassetteCacheTaskPriority = 1U;
static constexpr BaseType_t kCassetteCacheTaskCore = 1;
static constexpr uint32_t kCassetteTrimWaitMs = 1000U;

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
static lv_obj_t *g_tape_glints[4] = {};
static lv_point_precise_t g_tape_side_points[2][2] = {};
static lv_point_precise_t g_tape_middle_points[2] = {};
static lv_obj_t *g_title_label = nullptr;
static lv_obj_t *g_artist_label = nullptr;
static lv_obj_t *g_current_lyric_label = nullptr;
static lv_obj_t *g_next_lyric_label = nullptr;
static bool g_active = false;
static bool g_controls_visible = false;

// 磁带模式常驻 current + next 两张 460x460 压暗静态快照。
// current 用于展开控件立即显示；next 与下一首封面/变色壳同批预热，
// 控件页切歌时只交换指针，不再现场读盘、解码、找色、Tint 或合成压暗页。
static lv_obj_t *g_controls_cache_image = nullptr;
static uint8_t *g_controls_cache_pixels = nullptr;
static uint8_t *g_controls_cache_back_pixels = nullptr;
static uint8_t *g_next_controls_cache_pixels = nullptr;
static uint8_t *g_next_shell_rgb565 = nullptr;
static lv_image_dsc_t g_controls_cache_dsc = {};
static bool g_controls_cache_ready = false;
static bool g_controls_cache_dirty = true;
static uint32_t g_controls_cache_generation = 0U;
static uint32_t g_controls_cache_track = UINT32_MAX;

enum class CassetteCacheBuildRole : uint8_t
{
    Current = 0,
    Next,
};

struct CassetteCacheBuildJob
{
    uint32_t serial = 0U;
    CassetteCacheBuildRole role = CassetteCacheBuildRole::Current;
    uint32_t generation = 0U;
    uint32_t track = UINT32_MAX;
    bool dynamic_tint = false;
    bool no_artwork = false;
    int16_t cover_y_offset_px = 0;
    CoverSurfaceLease cover_lease = {};
    FallbackCoverImageLease fallback_lease = {};
    uint8_t *controls_output = nullptr;
    uint8_t *shell_output = nullptr;
};

struct CassetteCacheBuildResult
{
    uint32_t serial = 0U;
    CassetteCacheBuildRole role = CassetteCacheBuildRole::Current;
    uint32_t generation = 0U;
    uint32_t track = UINT32_MAX;
    bool ok = false;
    bool dynamic_tint = false;
    bool no_artwork = false;
    uint16_t tint_lut[256] = {};
    FallbackCoverImageLease fallback_lease = {};
    int64_t elapsed_us = 0LL;
};

enum class CassetteNextPrefetchState : uint8_t
{
    Idle = 0,
    WaitingArtwork,
    WaitingSurface,
    Building,
    Ready,
};

struct CassetteNextPrefetch
{
    CassetteNextPrefetchState state = CassetteNextPrefetchState::Idle;
    uint32_t generation = 0U;
    uint32_t track = UINT32_MAX;
    uint32_t serial = 0U;
    uint32_t artwork_request_id = 0U;
    uint32_t surface_request_id = 0U;
    bool dynamic_tint = false;
    bool no_artwork = false;
    CoverSurfaceLease promotion_cover = {};
    FallbackCoverImageLease promotion_fallback = {};
    uint16_t tint_lut[256] = {};
};

static CassetteNextPrefetch g_next_prefetch = {};
static QueueHandle_t g_cache_build_queue = nullptr;
static QueueHandle_t g_cache_result_queue = nullptr;
static TaskHandle_t g_cache_task = nullptr;
static bool g_cache_build_busy = false;
static uint32_t g_cache_build_serial = 0U;

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
// 着色工作区只保存 RGB565 平面，不直接暴露给 LVGL。
// 分块计算完成后一次提交到显示缓冲，避免屏幕读取到半成品。
static uint8_t *g_shell_tint_work_rgb565 = nullptr;
static uint8_t *g_shell_tint_luma = nullptr;
static uint32_t g_shell_tintable_pixels = 0U;
// 下面两个字段只表示“已经完整提交到屏幕”的配色版本。
static uint32_t g_shell_tint_generation = 0U;
static uint32_t g_shell_tint_track = UINT32_MAX;
static bool g_tint_setting_initialized = false;
static bool g_tint_setting_dynamic = false;

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

// 缓存上一次真正提交给 LVGL 的 Sprite 帧，避免相位未跨帧时重复 set_x。
static uint8_t g_last_big_reel_frame[2] = {0xFFU, 0xFFU};
static uint8_t g_last_small_roller_frame = 0xFFU;

// 高光视觉只由 phase + seeking 决定；快照未变化时整段跳过。
static uint8_t g_last_glint_snapshot = 0xFFU;

// 壳体着色与机械动画完全解耦。着色 timer 只在换色期间短暂运行，
// 每次处理有限像素，完成后一次提交到 LVGL 当前显示缓冲。
static constexpr uint32_t kShellTintChunkSize = 8192U;
static constexpr uint32_t kShellTintTimerPeriodMs = 20U;
struct ShellTintJob {
    bool in_progress = false;
    uint32_t generation = 0U;
    uint32_t track = UINT32_MAX;
    uint32_t current_index = 0U;
    uint16_t tint_lut[256] = {};
    int64_t started_us = 0LL;
};
static ShellTintJob g_shell_tint_job = {};
static lv_timer_t *g_shell_tint_timer = nullptr;

static CoverSurfaceLease g_cover_lease = {};
static FallbackCoverImageLease g_fallback_cover_lease = {};
static lv_image_dsc_t g_cover_dsc = {};
static uint32_t g_cover_generation = 0U;
static uint32_t g_cover_track = UINT32_MAX;
static uint32_t g_cover_scale_q8 = kLvImageScaleNone;
static int16_t g_cover_y_offset_px = 0;
static bool g_cover_is_no_artwork_fallback = false;

// 曲库会在列表停留期间停用磁带并释放常驻 lease。用户真正点歌前再短暂 pin 当前封面，
// 防止新歌 CoverTask 启动后把旧 Surface 当成未使用缓存提前回收。
static CoverSurfaceLease g_transition_hold_lease = {};

// 变色模式下，新封面先只 pin 在 pending lease 中，不立刻绑定到 LVGL。
// 等离屏壳体 TintJob 完整结束后，再在同一个 LVGL 回调内一次提交封面、壳体和小滚轮。
static CoverSurfaceLease g_pending_cover_lease = {};
static uint32_t g_pending_cover_generation = 0U;
static uint32_t g_pending_cover_track = UINT32_MAX;
static int16_t g_pending_cover_y_offset_px = 0;
static bool g_pending_cover_valid = false;
// 从封面视图首次切到磁带时，先在隐藏状态准备完整视觉，防止粉色壳体闪现。
static bool g_present_deferred = false;

static void cassette_view_init_rgb565_dsc(
    lv_image_dsc_t *dsc,
    const uint8_t *data,
    uint16_t width,
    uint16_t height,
    size_t size);
static void cassette_view_restore_shell_default();
static void cassette_view_restore_small_roller_default();
static bool cassette_view_prepare_small_roller_tint_assets();
static void cassette_view_cancel_shell_tint_job();
static void cassette_view_tick_shell_tint_chunk();
static void cassette_view_sync_tint_setting();
static void cassette_view_release_pending_cover();
static bool cassette_view_commit_pending_cover(uint32_t generation, uint32_t track);
static bool cassette_view_current_visual_ready();
static void cassette_view_mark_controls_cache_dirty();
static void cassette_view_handle_cache_result();
static void cassette_view_service_cache_pipeline();
static void cassette_view_cancel_next_prefetch(const char *reason);
static bool cassette_view_try_promote_next_visual(uint32_t generation, uint32_t track);

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
    cassette_view_mark_controls_cache_dirty();
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
    const bool controls_covering =
        g_controls_visible && g_controls_cache_ready && g_controls_cache_image != nullptr &&
        !lv_obj_has_flag(g_controls_cache_image, LV_OBJ_FLAG_HIDDEN);
    const bool visible = g_active && !controls_covering;
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
    if (!lyrics_service_is_ready() || !player_state_is_ready() ||
        media_library_get_count() == 0U) {
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

    // Controls 显示期间只提前加载当前曲歌词，不更新隐藏中的歌词标签。
    // 这样 LRC 可以与控件展示时间重叠，控件关闭后若已 Ready 就能直接显示。
    if (g_controls_visible) {
        return;
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
    const bool before_first_line = window.current_line_index == UINT32_MAX;

    // 歌曲开头尚未到第一句时间戳时，磁带页先预显示第一句，避免刚进入视图时首行空白。
    // 到达第一句时间戳后仍按歌词服务的 current 标记正常同步，不改变全局歌词时序。
    const char *current_text =
        current.valid && (current.current || before_first_line) ? current.text : "";
    char formatted_current[LYRICS_VIEW_TEXT_BYTES + 8U] = {};
    const bool current_two_lines =
        lyrics_text_measure_width(current_text) > kMiniLyricTextMaxW;
    lyrics_text_format_balanced(
        current_text,
        kMiniLyricTextMaxW,
        formatted_current,
        sizeof(formatted_current));

    cassette_view_set_text_if_changed(g_current_lyric_label, formatted_current);
    cassette_view_set_text_if_changed(
        g_next_lyric_label,
        !current_two_lines && next.valid ? next.text : "");
}

static void cassette_view_release_pending_cover()
{
    if (g_pending_cover_lease.slot_index != 0xFFU) {
        cover_surface_cache_release(&g_pending_cover_lease);
    }
    g_pending_cover_lease = {};
    g_pending_cover_generation = 0U;
    g_pending_cover_track = UINT32_MAX;
    g_pending_cover_y_offset_px = 0;
    g_pending_cover_valid = false;
}

static void cassette_view_release_transition_hold()
{
    if (g_transition_hold_lease.slot_index != 0xFFU) {
        ESP_LOGI(TAG, "磁带曲库交接释放：track=%lu revision=%lu",
            static_cast<unsigned long>(g_transition_hold_lease.track_index),
            static_cast<unsigned long>(g_transition_hold_lease.slot_revision));
        cover_surface_cache_release(&g_transition_hold_lease);
    }
    g_transition_hold_lease = {};
}

static void cassette_view_mark_controls_cache_dirty()
{
    // 控件打开时，当前 PSRAM 图就是“屏幕保持层”。新歌尚未准备完整时必须继续显示旧图，
    // 不能因为 PlayerState 已先切到新 track 就把旧图隐藏并退回实时 Alpha Backdrop。
    // dirty 只表示需要后台重建；真正的新缓存 ready 后再原子换源。
    g_controls_cache_dirty = true;
    if (!g_controls_visible && g_controls_cache_image != nullptr) {
        lv_obj_add_flag(g_controls_cache_image, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool cassette_view_controls_cache_matches_visible_visual()
{
    if (!g_controls_cache_ready || g_controls_cache_dirty ||
        g_cover_generation == 0U || g_cover_track == UINT32_MAX) {
        return false;
    }
    return g_controls_cache_generation == g_cover_generation &&
        g_controls_cache_track == g_cover_track;
}

static bool cassette_view_controls_cache_matches_current()
{
    if (!cassette_view_controls_cache_matches_visible_visual() ||
        !player_state_is_ready() || media_library_get_count() == 0U) {
        return false;
    }
    return g_cover_generation == media_catalog_v2_generation() &&
        g_cover_track == static_cast<uint32_t>(player_state_get_index());
}

static bool cassette_view_ensure_cache_buffer(uint8_t **buffer, size_t bytes, const char *name)
{
    if (buffer == nullptr) return false;
    if (*buffer != nullptr) return true;
    *buffer = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16U, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (*buffer == nullptr) {
        ESP_LOGW(TAG, "%s PSRAM申请失败：%uB",
            name != nullptr ? name : "磁带缓存", static_cast<unsigned>(bytes));
        return false;
    }
    return true;
}

static void cassette_view_cancel_next_prefetch(const char *reason)
{
    const uint32_t old_track = g_next_prefetch.track;
    if (g_next_prefetch.promotion_cover.slot_index != 0xFFU) {
        cover_surface_cache_release(&g_next_prefetch.promotion_cover);
    }
    if (g_next_prefetch.promotion_fallback.slot_index != 0xFFU) {
        fallback_cover_image_release(&g_next_prefetch.promotion_fallback);
    }
    g_next_prefetch = {};
    // serial 自增使正在后台合成的旧结果自动失效；next 快照/壳体 buffer 固定复用，到退出磁带时统一释放。
    ++g_cache_build_serial;
    if (g_cache_build_serial == 0U) g_cache_build_serial = 1U;
    if (old_track != UINT32_MAX && reason != nullptr) {
        ESP_LOGI(TAG, "下一首磁带预缓存取消：track=%lu reason=%s",
            static_cast<unsigned long>(old_track), reason);
    }
}

static uint32_t cassette_view_cover_scale_for_size(uint16_t width, uint16_t height)
{
    if (width == 0U || height == 0U) return kLvImageScaleNone;
    const uint32_t width_scale = static_cast<uint32_t>(
        (static_cast<uint64_t>(kCoverBleedWidth) * kLvImageScaleNone + width - 1U) / width);
    const uint32_t min_cover_height =
        static_cast<uint32_t>(kLabelHeight + 2 * kCoverEdgeSafetyPx);
    const uint32_t height_scale = static_cast<uint32_t>(
        (static_cast<uint64_t>(min_cover_height) * kLvImageScaleNone + height - 1U) / height);
    const uint32_t scale = width_scale > height_scale ? width_scale : height_scale;
    return scale == 0U ? 1U : scale;
}

static bool cassette_view_apply_transition_hold()
{
    if (g_transition_hold_lease.slot_index == 0xFFU || g_cover_image == nullptr ||
        g_transition_hold_lease.normal_rgb565 == nullptr ||
        g_transition_hold_lease.width == 0U || g_transition_hold_lease.height == 0U ||
        g_transition_hold_lease.data_size == 0U ||
        g_cover_lease.slot_index != 0xFFU || g_fallback_cover_lease.slot_index != 0xFFU) {
        return false;
    }

    g_cover_lease = g_transition_hold_lease;
    g_transition_hold_lease = {};
    g_cover_generation = g_cover_lease.catalog_generation;
    g_cover_track = g_cover_lease.track_index;
    g_cover_is_no_artwork_fallback = false;

    cassette_view_init_rgb565_dsc(
        &g_cover_dsc,
        g_cover_lease.normal_rgb565,
        g_cover_lease.width,
        g_cover_lease.height,
        g_cover_lease.data_size);
    g_cover_scale_q8 = cassette_view_cover_scale_for_size(
        g_cover_lease.width, g_cover_lease.height);
    lv_image_set_src(g_cover_image, &g_cover_dsc);
    lv_image_set_scale(g_cover_image, g_cover_scale_q8);
    lv_image_set_antialias(g_cover_image, false);
    cassette_view_apply_cover_position();
    lv_obj_remove_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "磁带曲库交接恢复旧封面：track=%lu revision=%lu",
        static_cast<unsigned long>(g_cover_track),
        static_cast<unsigned long>(g_cover_lease.slot_revision));
    return true;
}

static bool cassette_view_commit_pending_cover(uint32_t generation, uint32_t track)
{
    if (!g_pending_cover_valid || g_pending_cover_lease.slot_index == 0xFFU ||
        g_pending_cover_generation != generation || g_pending_cover_track != track ||
        g_pending_cover_lease.normal_rgb565 == nullptr) {
        return false;
    }

    // Launcher 临时隐藏只会释放当前 Surface lease，不代表磁带视觉发生变化。
    // 若恢复的仍是同一首、现有 Controls 缓存也仍匹配，就只重新绑定封面，
    // 不把 Controls 误标为 dirty，否则会无谓取消已经准备好的 next 预缓存。
    const bool keep_controls_cache =
        g_cover_generation == generation && g_cover_track == track &&
        cassette_view_controls_cache_matches_visible_visual();

    CoverSurfaceLease old_cover = g_cover_lease;
    FallbackCoverImageLease old_fallback = g_fallback_cover_lease;

    g_cover_lease = g_pending_cover_lease;
    g_pending_cover_lease = {};
    g_fallback_cover_lease = {};
    g_cover_generation = generation;
    g_cover_track = track;
    g_cover_y_offset_px = g_pending_cover_y_offset_px;
    g_cover_is_no_artwork_fallback = false;
    g_pending_cover_generation = 0U;
    g_pending_cover_track = UINT32_MAX;
    g_pending_cover_y_offset_px = 0;
    g_pending_cover_valid = false;

    cassette_view_init_rgb565_dsc(
        &g_cover_dsc,
        g_cover_lease.normal_rgb565,
        g_cover_lease.width,
        g_cover_lease.height,
        g_cover_lease.data_size);
    g_cover_scale_q8 = cassette_view_cover_scale_for_size(
        g_cover_lease.width, g_cover_lease.height);
    lv_image_set_src(g_cover_image, &g_cover_dsc);
    lv_image_set_scale(g_cover_image, g_cover_scale_q8);
    lv_image_set_antialias(g_cover_image, false);
    cassette_view_apply_cover_position();
    lv_obj_remove_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);

    if (old_cover.slot_index != 0xFFU) {
        cover_surface_cache_release(&old_cover);
    }
    if (old_fallback.slot_index != 0xFFU) {
        fallback_cover_image_release(&old_fallback);
        fallback_cover_image_discard_unpinned();
    }
    if (!keep_controls_cache) {
        cassette_view_mark_controls_cache_dirty();
    }
    return true;
}

static bool cassette_view_bind_no_artwork_label(uint32_t generation, uint32_t track)
{
    if (g_cover_image == nullptr) return false;

    // 无封面是一个明确的完整视觉状态：专用标签封面 + 原装粉色壳体/小滚轮。
    // 先把 fallback 图片准备好，再在同一个 LVGL 回调内一起提交，避免先闪上一首或半套状态。
    FallbackCoverImageLease next_fallback = {};
    const bool acquired = fallback_cover_image_acquire(
        FallbackCoverImageKind::Cassette, &next_fallback);
    const bool fallback_ready = acquired && next_fallback.rgb565 != nullptr &&
        next_fallback.width > 0U && next_fallback.height > 0U;

    cassette_view_cancel_shell_tint_job();
    cassette_view_release_pending_cover();

    CoverSurfaceLease old_cover = g_cover_lease;
    FallbackCoverImageLease old_fallback = g_fallback_cover_lease;
    g_cover_lease = {};
    g_fallback_cover_lease = next_fallback;
    g_cover_dsc = {};
    g_cover_scale_q8 = kLvImageScaleNone;
    if (g_cover_track != track) {
        g_cover_y_offset_px = 0;
    }
    g_cover_generation = generation;
    g_cover_track = track;
    g_cover_is_no_artwork_fallback = true;

    // 真正无封面时恢复产品默认粉色，不让替补标签纸参与壳体/小轮取色。
    cassette_view_restore_shell_default();
    cassette_view_restore_small_roller_default();

    if (fallback_ready) {
        cassette_view_init_rgb565_dsc(
            &g_cover_dsc,
            g_fallback_cover_lease.rgb565,
            g_fallback_cover_lease.width,
            g_fallback_cover_lease.height,
            g_fallback_cover_lease.data_size);
        lv_image_set_src(g_cover_image, &g_cover_dsc);
        g_cover_scale_q8 = cassette_view_cover_scale_for_size(
            g_fallback_cover_lease.width, g_fallback_cover_lease.height);
        lv_image_set_scale(g_cover_image, g_cover_scale_q8);
        lv_image_set_antialias(g_cover_image, false);
        cassette_view_apply_cover_position();
        lv_obj_remove_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG,
            "磁带标签使用TF替补封面：track=%lu path=/sdcard/System/no_cover_cassette.jpg scale=%u/256",
            static_cast<unsigned long>(track),
            static_cast<unsigned>(g_cover_scale_q8));
    } else {
        // 文件缺失/尺寸错误时只保留黑色 Label 底，不退回代码绘制替补。
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
    cassette_view_mark_controls_cache_dirty();
    return true;
}

static void cassette_view_release_cover(bool preserve_visual_identity)
{
    cassette_view_cancel_shell_tint_job();
    cassette_view_release_pending_cover();
    if (g_cover_lease.slot_index != 0xFFU) {
        cover_surface_cache_release(&g_cover_lease);
    }
    g_cover_lease = {};

    // Lyrics/Spectrum/曲库/Launcher/Settings 都只是临时覆盖 Music。
    // 当前曲若使用固定缺省封面，保留唯一 fallback lease 和绑定描述符；父 root 已隐藏，
    // 恢复时可直接重新显示，不再次读 SD + JPEG 解码，也不制造一帧黑标签。
    const bool keep_current_fallback = preserve_visual_identity &&
        g_cover_is_no_artwork_fallback &&
        g_fallback_cover_lease.slot_index != 0xFFU &&
        g_fallback_cover_lease.rgb565 != nullptr;
    if (!keep_current_fallback) {
        if (g_fallback_cover_lease.slot_index != 0xFFU) {
            fallback_cover_image_release(&g_fallback_cover_lease);
        }
        g_fallback_cover_lease = {};
        g_cover_dsc = {};
        g_cover_scale_q8 = kLvImageScaleNone;
        if (g_cover_image != nullptr) {
            lv_obj_add_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 临时接管屏幕时保留上一套已提交视觉身份；真正离开磁带视图才清空。
    if (!preserve_visual_identity) {
        g_cover_generation = 0U;
        g_cover_track = UINT32_MAX;
        g_cover_is_no_artwork_fallback = false;
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
        if (big_frames[i] == g_last_big_reel_frame[i]) continue;
        if (g_big_reel_strip_images[i] != nullptr) {
            lv_obj_set_x(g_big_reel_strip_images[i],
                -static_cast<int32_t>(big_frames[i]) * kBigReelSize);
        }
        g_last_big_reel_frame[i] = big_frames[i];
    }

    // 8 点小轮的视觉周期为 45°，0° / 22.5° 两帧已经覆盖全部唯一相位。
    // 因为只有两个唯一相位，正/反方向在视觉上等价，两侧直接共用同一帧。
    small_frame %= kSmallRollerFrameCount;
    if (small_frame != g_last_small_roller_frame) {
        for (size_t i = 0U; i < 2U; ++i) {
            if (g_small_roller_strip_images[i] != nullptr) {
                lv_obj_set_x(g_small_roller_strip_images[i],
                    -static_cast<int32_t>(small_frame) * kSmallRollerSize);
            }
        }
        g_last_small_roller_frame = small_frame;
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
    const uint8_t snapshot = static_cast<uint8_t>(phase * 2U + (seeking ? 1U : 0U));
    if (snapshot == g_last_glint_snapshot) return;
    g_last_glint_snapshot = snapshot;

    // 斜线高光保留少量垂直抖动；中间横线只做水平扫光，避免带面上下跳。
    static constexpr int8_t kTravel[4] = {0, 1, 2, 3};
    static constexpr int8_t kJitterY[4] = {0, -1, 0, 1};
    static constexpr uint8_t kSideOpa[4] = {125U, 205U, 165U, 105U};
    const int16_t travel = static_cast<int16_t>(kTravel[phase]) * (seeking ? 2 : 1);
    const int16_t side_jitter_y =
        static_cast<int16_t>(kJitterY[phase]) * (seeking ? 2 : 1);

    for (size_t i = 0U; i < 2U; ++i) {
        lv_obj_t *dot = g_tape_glints[i];
        if (dot == nullptr) continue;
        const int32_t t = 16 + static_cast<int32_t>(kTravel[phase]) * (seeking ? 2 : 1);
        const lv_point_precise_t p = cassette_view_interpolate_point(
            g_tape_side_points[i][0], g_tape_side_points[i][1], t, 32);
        lv_obj_set_pos(
            dot, static_cast<int16_t>(p.x), static_cast<int16_t>(p.y + side_jitter_y));
        uint8_t opacity = kSideOpa[phase];
        if (seeking && opacity <= 225U) opacity += 20U;
        lv_obj_set_style_bg_opa(dot, opacity, 0);
    }

    // 两个中间高光点使用反相透明度，形成平滑横向扫光。
    static constexpr int16_t kMiddleBaseX[2] = {180, 280};
    static constexpr uint8_t kMiddleLeadingOpa[4] = {205U, 165U, 105U, 70U};
    static constexpr uint8_t kMiddleTrailingOpa[4] = {70U, 105U, 165U, 205U};
    for (size_t i = 0U; i < 2U; ++i) {
        lv_obj_t *dot = g_tape_glints[i + 2U];
        if (dot == nullptr) continue;
        lv_obj_set_pos(
            dot,
            kCassetteX + kMiddleBaseX[i] + travel,
            kCassetteY + kTapeSmallBottomY - 1);
        uint8_t opacity =
            (i == 0U ? kMiddleLeadingOpa : kMiddleTrailingOpa)[phase];
        if (seeking && opacity <= 225U) opacity += 20U;
        lv_obj_set_style_bg_opa(dot, opacity, 0);
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
    // 几何发生变化后即使高光相位没变，也必须在下一次高光刷新时重新定位。
    g_last_glint_snapshot = 0xFFU;

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

}

static void cassette_view_set_tape_path_visible(bool visible)
{
    lv_obj_t *objects[] = {
        g_tape_side_lines[0], g_tape_side_lines[1], g_tape_middle_line,
        g_tape_glints[0], g_tape_glints[1], g_tape_glints[2],
        g_tape_glints[3],
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
    g_last_big_reel_frame[0] = 0xFFU;
    g_last_big_reel_frame[1] = 0xFFU;
    g_last_small_roller_frame = 0xFFU;
    g_last_glint_snapshot = 0xFFU;
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
    ESP_LOGI(TAG, "走带线已准备：3段/2px 粉棕主线 + 4个像素高光，10Hz跳动");
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

static inline uint16_t cassette_view_blend_rgb565(uint16_t background, uint16_t foreground, uint8_t alpha)
{
    if (alpha == 0U) return background;
    if (alpha == 255U) return foreground;
    const uint32_t inv = 255U - alpha;
    const uint32_t br = (background >> 11U) & 0x1FU;
    const uint32_t bg = (background >> 5U) & 0x3FU;
    const uint32_t bb = background & 0x1FU;
    const uint32_t fr = (foreground >> 11U) & 0x1FU;
    const uint32_t fg = (foreground >> 5U) & 0x3FU;
    const uint32_t fb = foreground & 0x1FU;
    const uint32_t rr = (fr * alpha + br * inv + 127U) / 255U;
    const uint32_t rg = (fg * alpha + bg * inv + 127U) / 255U;
    const uint32_t rb = (fb * alpha + bb * inv + 127U) / 255U;
    return static_cast<uint16_t>((rr << 11U) | (rg << 5U) | rb);
}

static inline uint16_t cassette_view_hex_to_rgb565(uint32_t color_hex)
{
    const uint8_t r = static_cast<uint8_t>((color_hex >> 16U) & 0xFFU);
    const uint8_t g = static_cast<uint8_t>((color_hex >> 8U) & 0xFFU);
    const uint8_t b = static_cast<uint8_t>(color_hex & 0xFFU);
    return static_cast<uint16_t>(
        ((static_cast<uint16_t>(r) & 0xF8U) << 8U) |
        ((static_cast<uint16_t>(g) & 0xFCU) << 3U) |
        (static_cast<uint16_t>(b) >> 3U));
}

static void cassette_view_put_pixel(
    uint16_t *dst, int16_t x, int16_t y, uint16_t color, uint8_t alpha = 255U)
{
    if (dst == nullptr || x < 0 || y < 0 ||
        x >= static_cast<int16_t>(kControlsCacheWidth) ||
        y >= static_cast<int16_t>(kControlsCacheHeight)) {
        return;
    }
    uint16_t &pixel = dst[static_cast<size_t>(y) * kControlsCacheWidth + x];
    pixel = cassette_view_blend_rgb565(pixel, color, alpha);
}

static void cassette_view_blit_rgb565a8_frame(
    uint16_t *dst,
    const uint8_t *native,
    uint16_t source_width,
    uint16_t source_height,
    uint16_t frame_x,
    uint16_t frame_width,
    int16_t dst_x,
    int16_t dst_y)
{
    if (dst == nullptr || native == nullptr || frame_width == 0U ||
        frame_x + frame_width > source_width) {
        return;
    }
    const size_t pixel_count = static_cast<size_t>(source_width) * source_height;
    const uint8_t *alpha_plane = native + pixel_count * 2U;
    for (uint16_t y = 0U; y < source_height; ++y) {
        const int16_t out_y = static_cast<int16_t>(dst_y + y);
        if (out_y < 0 || out_y >= static_cast<int16_t>(kControlsCacheHeight)) continue;
        for (uint16_t x = 0U; x < frame_width; ++x) {
            const int16_t out_x = static_cast<int16_t>(dst_x + x);
            if (out_x < 0 || out_x >= static_cast<int16_t>(kControlsCacheWidth)) continue;
            const size_t source_index =
                static_cast<size_t>(y) * source_width + frame_x + x;
            const uint8_t alpha = alpha_plane[source_index];
            if (alpha == 0U) continue;
            const uint16_t color = cassette_view_load_rgb565(native + source_index * 2U);
            cassette_view_put_pixel(dst, out_x, out_y, color, alpha);
        }
        if ((y & 0x1FU) == 0x1FU) vTaskDelay(1);
    }
}

static void cassette_view_blit_cover(
    uint16_t *dst,
    const uint8_t *source,
    uint16_t source_width,
    uint16_t source_height,
    int16_t y_offset_px)
{
    if (dst == nullptr || source == nullptr || source_width == 0U || source_height == 0U) return;
    const uint32_t scale = cassette_view_cover_scale_for_size(source_width, source_height);
    const int32_t scaled_width = static_cast<int32_t>(
        (static_cast<uint64_t>(source_width) * scale) / kLvImageScaleNone);
    const int32_t scaled_height = static_cast<int32_t>(
        (static_cast<uint64_t>(source_height) * scale) / kLvImageScaleNone);
    const int32_t left = kCassetteX + kLabelX + (kLabelWidth - scaled_width) / 2;
    const int32_t top = kCassetteY + kLabelY + (kLabelHeight - scaled_height) / 2 + y_offset_px;
    const int32_t clip_left = kCassetteX + kLabelX;
    const int32_t clip_top = kCassetteY + kLabelY;
    const int32_t clip_right = clip_left + kLabelWidth;
    const int32_t clip_bottom = clip_top + kLabelHeight;

    for (int32_t y = clip_top; y < clip_bottom; ++y) {
        const int32_t scaled_y = y - top;
        if (scaled_y < 0 || scaled_y >= scaled_height) continue;
        uint32_t source_y = static_cast<uint32_t>(
            (static_cast<uint64_t>(scaled_y) * kLvImageScaleNone) / scale);
        if (source_y >= source_height) source_y = source_height - 1U;
        for (int32_t x = clip_left; x < clip_right; ++x) {
            const int32_t scaled_x = x - left;
            if (scaled_x < 0 || scaled_x >= scaled_width) continue;
            uint32_t source_x = static_cast<uint32_t>(
                (static_cast<uint64_t>(scaled_x) * kLvImageScaleNone) / scale);
            if (source_x >= source_width) source_x = source_width - 1U;
            const size_t source_index =
                static_cast<size_t>(source_y) * source_width + source_x;
            dst[static_cast<size_t>(y) * kControlsCacheWidth + x] =
                cassette_view_load_rgb565(source + source_index * 2U);
        }
        if ((y & 0x1FU) == 0x1FU) vTaskDelay(1);
    }
}

static void cassette_view_blit_small_roller_tinted(
    uint16_t *dst,
    int16_t dst_x,
    int16_t dst_y,
    const uint16_t tint_lut[256],
    bool dynamic_tint)
{
    if (dst == nullptr || g_small_roller_pixels == nullptr) return;
    const uint16_t source_width = static_cast<uint16_t>(kSmallRollerSize * kSmallRollerFrameCount);
    const uint16_t source_height = kSmallRollerSize;
    const size_t pixel_count = static_cast<size_t>(source_width) * source_height;
    const uint8_t *alpha_plane = g_small_roller_pixels + pixel_count * 2U;
    const uint8_t *base = g_small_roller_base_rgb565 != nullptr
        ? g_small_roller_base_rgb565 : g_small_roller_pixels;

    for (uint16_t y = 0U; y < kSmallRollerSize; ++y) {
        for (uint16_t x = 0U; x < kSmallRollerSize; ++x) {
            const size_t source_index = static_cast<size_t>(y) * source_width + x;
            const uint8_t alpha = alpha_plane[source_index];
            if (alpha == 0U) continue;
            uint16_t color = cassette_view_load_rgb565(base + source_index * 2U);
            if (dynamic_tint && tint_lut != nullptr && g_small_roller_tint_luma != nullptr) {
                const uint8_t mapped = g_small_roller_tint_luma[source_index];
                if (mapped != 0U) color = tint_lut[mapped - 1U];
            }
            cassette_view_put_pixel(
                dst, static_cast<int16_t>(dst_x + x), static_cast<int16_t>(dst_y + y),
                color, alpha);
        }
    }
}

static void cassette_view_blit_shell(
    uint16_t *dst,
    uint8_t *shell_output,
    const uint16_t tint_lut[256],
    bool dynamic_tint)
{
    if ((dst == nullptr && shell_output == nullptr) ||
        g_shell_pixels == nullptr || g_shell_base_rgb565 == nullptr) return;
    const size_t pixel_count = static_cast<size_t>(kCassetteWidth) * kCassetteHeight;
    const uint8_t *alpha_plane = g_shell_pixels + pixel_count * 2U;
    for (int16_t y = 0; y < kCassetteHeight; ++y) {
        for (int16_t x = 0; x < kCassetteWidth; ++x) {
            const size_t index = static_cast<size_t>(y) * kCassetteWidth + x;
            uint16_t color = cassette_view_load_rgb565(g_shell_base_rgb565 + index * 2U);
            if (dynamic_tint && tint_lut != nullptr && g_shell_tint_luma != nullptr) {
                const uint8_t mapped = g_shell_tint_luma[index];
                if (mapped != 0U) color = tint_lut[mapped - 1U];
            }
            if (shell_output != nullptr) {
                cassette_view_store_rgb565(shell_output + index * 2U, color);
            }
            const uint8_t alpha = alpha_plane[index];
            if (dst != nullptr && alpha != 0U) {
                cassette_view_put_pixel(
                    dst, static_cast<int16_t>(kCassetteX + x),
                    static_cast<int16_t>(kCassetteY + y), color, alpha);
            }
        }
        if ((y & 0x1FU) == 0x1FU) vTaskDelay(1);
    }
}

static void cassette_view_draw_line(
    uint16_t *dst,
    int16_t x0,
    int16_t y0,
    int16_t x1,
    int16_t y1,
    uint16_t color,
    uint8_t alpha,
    uint8_t width)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t dy_abs = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t dy = -dy_abs;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    while (true) {
        for (uint8_t w = 0U; w < width; ++w) {
            cassette_view_put_pixel(dst, x0, static_cast<int16_t>(y0 + w), color, alpha);
        }
        if (x0 == x1 && y0 == y1) break;
        const int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 = static_cast<int16_t>(x0 + sx); }
        if (e2 <= dx) { err += dx; y0 = static_cast<int16_t>(y0 + sy); }
    }
}

static void cassette_view_draw_fixed_tape_path(uint16_t *dst)
{
    if (dst == nullptr) return;
    const int16_t big_x[2] = {
        kTapeBigLeftBaseX,
        static_cast<int16_t>(kTapeBigRightBaseX + kTapeBigOuterTravelPx),
    };
    const int16_t small_x[2] = {kTapeSmallLeftSideX, kTapeSmallRightSideX};
    int16_t visible_x[2] = {};
    for (size_t i = 0U; i < 2U; ++i) {
        const int32_t dy = kTapeSmallSideY - kTapeBigAnchorY;
        const int32_t visible_dy = kTapeShellVisibleTopY - kTapeBigAnchorY;
        const int32_t dx = static_cast<int32_t>(small_x[i]) - big_x[i];
        visible_x[i] = static_cast<int16_t>(
            static_cast<int32_t>(big_x[i]) + (dx * visible_dy) / dy);
        cassette_view_draw_line(
            dst,
            static_cast<int16_t>(kCassetteX + visible_x[i]),
            static_cast<int16_t>(kCassetteY + kTapeShellVisibleTopY),
            static_cast<int16_t>(kCassetteX + small_x[i]),
            static_cast<int16_t>(kCassetteY + kTapeSmallSideY),
            cassette_view_hex_to_rgb565(kTapeSideColorHex), kTapeSideOpa, kTapeLineWidth);
    }
    cassette_view_draw_line(
        dst,
        static_cast<int16_t>(kCassetteX + kTapeSmallLeftBottomX),
        static_cast<int16_t>(kCassetteY + kTapeSmallBottomY),
        static_cast<int16_t>(kCassetteX + kTapeSmallRightBottomX),
        static_cast<int16_t>(kCassetteY + kTapeSmallBottomY),
        cassette_view_hex_to_rgb565(kTapeMiddleColorHex), kTapeMiddleOpa, kTapeLineWidth);

    const uint16_t glint = cassette_view_hex_to_rgb565(kTapeGlintColorHex);
    for (size_t i = 0U; i < 2U; ++i) {
        const int32_t t = 16;
        const int16_t x = static_cast<int16_t>(
            kCassetteX + visible_x[i] +
            ((small_x[i] - visible_x[i]) * t) / 32);
        const int16_t y = static_cast<int16_t>(
            kCassetteY + kTapeShellVisibleTopY +
            ((kTapeSmallSideY - kTapeShellVisibleTopY) * t) / 32);
        for (int16_t yy = 0; yy < 2; ++yy) {
            for (int16_t xx = 0; xx < 2; ++xx) {
                cassette_view_put_pixel(dst, x + xx, y + yy, glint, 125U);
            }
        }
    }
    const uint8_t middle_opa[2] = {205U, 70U};
    const int16_t middle_x[2] = {180, 280};
    const int16_t middle_w[2] = {4, 3};
    for (size_t i = 0U; i < 2U; ++i) {
        for (int16_t yy = 0; yy < 2; ++yy) {
            for (int16_t xx = 0; xx < middle_w[i]; ++xx) {
                cassette_view_put_pixel(
                    dst,
                    static_cast<int16_t>(kCassetteX + middle_x[i] + xx),
                    static_cast<int16_t>(kCassetteY + kTapeSmallBottomY - 1 + yy),
                    glint, middle_opa[i]);
            }
        }
    }
}

static void cassette_view_dim_buffer(uint16_t *pixels)
{
    if (pixels == nullptr) return;
    const uint32_t keep = 255U - kControlsCacheDimOpacity;
    const size_t pixel_count =
        static_cast<size_t>(kControlsCacheWidth) * kControlsCacheHeight;
    for (size_t index = 0U; index < pixel_count; ++index) {
        const uint16_t color = pixels[index];
        const uint32_t r = (color >> 11U) & 0x1FU;
        const uint32_t g = (color >> 5U) & 0x3FU;
        const uint32_t b = color & 0x1FU;
        const uint16_t dr = static_cast<uint16_t>((r * keep + 127U) / 255U);
        const uint16_t dg = static_cast<uint16_t>((g * keep + 127U) / 255U);
        const uint16_t db = static_cast<uint16_t>((b * keep + 127U) / 255U);
        pixels[index] = static_cast<uint16_t>((dr << 11U) | (dg << 5U) | db);
        if ((index & 0x3FFFU) == 0x3FFFU) vTaskDelay(1);
    }
}

static bool cassette_view_compose_cache_job(
    const CassetteCacheBuildJob &job,
    CassetteCacheBuildResult *out_result)
{
    if (out_result == nullptr ||
        (job.controls_output == nullptr && job.shell_output == nullptr) ||
        g_shell_base_rgb565 == nullptr || g_shell_tint_luma == nullptr) {
        return false;
    }

    const bool build_controls = job.controls_output != nullptr;
    if (build_controls &&
        (!g_mechanics_ready || g_big_reel_pixels == nullptr ||
         g_small_roller_pixels == nullptr || g_tape_amount_pixels == nullptr)) {
        return false;
    }

    const uint8_t *cover = job.no_artwork
        ? job.fallback_lease.rgb565 : job.cover_lease.normal_rgb565;
    const uint16_t cover_width = job.no_artwork
        ? job.fallback_lease.width : job.cover_lease.width;
    const uint16_t cover_height = job.no_artwork
        ? job.fallback_lease.height : job.cover_lease.height;
    if (!job.no_artwork &&
        (cover == nullptr || cover_width == 0U || cover_height == 0U)) {
        return false;
    }

    CassetteTintColor extracted = {};
    CassetteTintColor target = kCassetteNeutralTint;
    uint16_t source_hue = 0U;
    uint16_t target_hue = 0U;
    uint32_t samples = 0U;
    const bool apply_dynamic = job.dynamic_tint && !job.no_artwork;
    if (apply_dynamic) {
        const bool chromatic = cassette_view_extract_cover_tint(
            job.cover_lease, &extracted, &source_hue, &samples);
        if (chromatic) {
            target = cassette_view_normalize_dynamic_tint(extracted, &target_hue);
        }
        cassette_view_build_tint_lut(target, out_result->tint_lut);
    }

    uint16_t *dst = build_controls
        ? reinterpret_cast<uint16_t *>(job.controls_output)
        : nullptr;
    if (build_controls) {
        memset(job.controls_output, 0, kControlsCacheBytes);
        if (cover != nullptr && cover_width > 0U && cover_height > 0U) {
            cassette_view_blit_cover(
                dst, cover, cover_width, cover_height, job.cover_y_offset_px);
        }

        cassette_view_blit_rgb565a8_frame(
            dst, g_tape_amount_pixels,
            static_cast<uint16_t>(kTapeAmountWidth), static_cast<uint16_t>(kTapeAmountHeight),
            0U, static_cast<uint16_t>(kTapeAmountWidth),
            static_cast<int16_t>(kCassetteX + kTapeAmountBaseX - kTapeAmountTravelPx),
            static_cast<int16_t>(kCassetteY + kTapeAmountY));
        cassette_view_blit_rgb565a8_frame(
            dst, g_big_reel_pixels,
            static_cast<uint16_t>(kBigReelSize * kBigReelFrameCount),
            static_cast<uint16_t>(kBigReelSize),
            0U, static_cast<uint16_t>(kBigReelSize),
            static_cast<int16_t>(kCassetteX + kBigReelLeftX),
            static_cast<int16_t>(kCassetteY + kBigReelY));
        cassette_view_blit_rgb565a8_frame(
            dst, g_big_reel_pixels,
            static_cast<uint16_t>(kBigReelSize * kBigReelFrameCount),
            static_cast<uint16_t>(kBigReelSize),
            0U, static_cast<uint16_t>(kBigReelSize),
            static_cast<int16_t>(kCassetteX + kBigReelRightX),
            static_cast<int16_t>(kCassetteY + kBigReelY));
        cassette_view_blit_small_roller_tinted(
            dst,
            static_cast<int16_t>(kCassetteX + kSmallRollerLeftX),
            static_cast<int16_t>(kCassetteY + kSmallRollerY),
            out_result->tint_lut, apply_dynamic);
        cassette_view_blit_small_roller_tinted(
            dst,
            static_cast<int16_t>(kCassetteX + kSmallRollerRightX),
            static_cast<int16_t>(kCassetteY + kSmallRollerY),
            out_result->tint_lut, apply_dynamic);
    }

    // current/next 都生成 460x460 压暗静态磁带；next 同时生成已变色磁带壳。
    cassette_view_blit_shell(
        dst, job.shell_output, out_result->tint_lut, apply_dynamic);
    if (build_controls) {
        cassette_view_draw_fixed_tape_path(dst);
        cassette_view_dim_buffer(dst);
    }

    out_result->dynamic_tint = apply_dynamic;
    return true;
}

static void cassette_view_cache_task_main(void *argument)
{
    (void)argument;
    while (true) {
        CassetteCacheBuildJob job = {};
        if (xQueueReceive(g_cache_build_queue, &job, portMAX_DELAY) != pdTRUE) continue;

        CassetteCacheBuildResult result = {};
        result.serial = job.serial;
        result.role = job.role;
        result.generation = job.generation;
        result.track = job.track;
        result.no_artwork = job.no_artwork;
        // 下一首无封面也在 Core1 低优先级任务里提前读 SD + 解 JPEG。
        // 这样不会把缺省封面的首次解码延迟挪到真正切歌那一帧。
        if (job.no_artwork && job.fallback_lease.slot_index == 0xFFU) {
            (void)fallback_cover_image_acquire(
                FallbackCoverImageKind::Cassette, &job.fallback_lease);
        }
        result.fallback_lease = job.fallback_lease;
        const int64_t started_us = esp_timer_get_time();
        result.ok = cassette_view_compose_cache_job(job, &result);
        result.elapsed_us = esp_timer_get_time() - started_us;

        if (job.cover_lease.slot_index != 0xFFU) {
            CoverSurfaceLease release = job.cover_lease;
            cover_surface_cache_release(&release);
        }
        (void)xQueueOverwrite(g_cache_result_queue, &result);
    }
}

static bool cassette_view_start_cache_task()
{
    if (g_cache_task != nullptr) return true;
    if (g_cache_build_queue == nullptr) {
        g_cache_build_queue = xQueueCreate(1U, sizeof(CassetteCacheBuildJob));
    }
    if (g_cache_result_queue == nullptr) {
        g_cache_result_queue = xQueueCreate(1U, sizeof(CassetteCacheBuildResult));
    }
    if (g_cache_build_queue == nullptr || g_cache_result_queue == nullptr) return false;

    const BaseType_t created = xTaskCreatePinnedToCore(
        cassette_view_cache_task_main,
        "CassetteCache",
        kCassetteCacheTaskStackBytes,
        nullptr,
        kCassetteCacheTaskPriority,
        &g_cache_task,
        kCassetteCacheTaskCore);
    if (created != pdPASS) {
        g_cache_task = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "磁带预缓存任务已启动：Core=%d Priority=%u current+next压暗快照+next壳体预取",
        static_cast<int>(kCassetteCacheTaskCore),
        static_cast<unsigned>(kCassetteCacheTaskPriority));
    return true;
}

static bool cassette_view_submit_cache_job(CassetteCacheBuildJob *job)
{
    if (job == nullptr || g_cache_build_busy || g_cache_task == nullptr ||
        g_cache_build_queue == nullptr) {
        return false;
    }
    ++g_cache_build_serial;
    if (g_cache_build_serial == 0U) g_cache_build_serial = 1U;
    job->serial = g_cache_build_serial;
    if (xQueueOverwrite(g_cache_build_queue, job) != pdTRUE) return false;
    g_cache_build_busy = true;
    if (job->role == CassetteCacheBuildRole::Next) {
        g_next_prefetch.serial = job->serial;
        g_next_prefetch.state = CassetteNextPrefetchState::Building;
    }
    return true;
}

static void cassette_view_promote_controls_buffer(uint32_t generation, uint32_t track)
{
    uint8_t *old_current = g_controls_cache_pixels;
    g_controls_cache_pixels = g_controls_cache_back_pixels;
    g_controls_cache_back_pixels = nullptr;
    if (g_controls_cache_pixels == nullptr) return;

    cassette_view_init_rgb565_dsc(
        &g_controls_cache_dsc,
        g_controls_cache_pixels,
        kControlsCacheWidth,
        kControlsCacheHeight,
        kControlsCacheBytes);
    lv_image_set_src(g_controls_cache_image, &g_controls_cache_dsc);
    lv_image_set_antialias(g_controls_cache_image, false);
    g_controls_cache_generation = generation;
    g_controls_cache_track = track;
    g_controls_cache_dirty = false;
    g_controls_cache_ready = true;
    if (g_controls_visible) {
        lv_obj_remove_flag(g_controls_cache_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_controls_cache_image);
    }
    // back buffer 只用于原子生成；提升完成后旧前台图立即释放，不长期保留第二张 460x460。
    if (old_current != nullptr) heap_caps_free(old_current);
    cassette_view_apply_aux_visibility();
}

static bool cassette_view_promote_next_controls_buffer(uint32_t generation, uint32_t track)
{
    if (g_next_controls_cache_pixels == nullptr) return false;

    // next 快照已经完整离屏生成。直接交换 current/next 指针，旧 current buffer
    // 留作下一轮 next 的复用缓冲，切歌路径不 malloc/free 413KiB 大块。
    uint8_t *old_current = g_controls_cache_pixels;
    g_controls_cache_pixels = g_next_controls_cache_pixels;
    g_next_controls_cache_pixels = old_current;

    cassette_view_init_rgb565_dsc(
        &g_controls_cache_dsc,
        g_controls_cache_pixels,
        kControlsCacheWidth,
        kControlsCacheHeight,
        kControlsCacheBytes);
    lv_image_set_src(g_controls_cache_image, &g_controls_cache_dsc);
    lv_image_set_antialias(g_controls_cache_image, false);
    g_controls_cache_generation = generation;
    g_controls_cache_track = track;
    g_controls_cache_dirty = false;
    g_controls_cache_ready = true;
    if (g_controls_visible) {
        lv_obj_remove_flag(g_controls_cache_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_controls_cache_image);
    } else {
        lv_obj_add_flag(g_controls_cache_image, LV_OBJ_FLAG_HIDDEN);
    }
    cassette_view_apply_aux_visibility();
    return true;
}

static void cassette_view_handle_cache_result()
{
    if (g_cache_result_queue == nullptr) return;
    CassetteCacheBuildResult result = {};
    if (xQueueReceive(g_cache_result_queue, &result, 0) != pdTRUE) return;
    g_cache_build_busy = false;

    if (result.serial != g_cache_build_serial) {
        if (result.fallback_lease.slot_index != 0xFFU) {
            fallback_cover_image_release(&result.fallback_lease);
        }
        ESP_LOGI(TAG, "磁带预缓存丢弃过期结果：track=%lu role=%s",
            static_cast<unsigned long>(result.track),
            result.role == CassetteCacheBuildRole::Next ? "next" : "current");
        return;
    }

    if (result.role == CassetteCacheBuildRole::Current) {
        if (result.fallback_lease.slot_index != 0xFFU) {
            fallback_cover_image_release(&result.fallback_lease);
        }
        const bool still_current = player_state_is_ready() && media_library_get_count() > 0U &&
            media_catalog_v2_generation() == result.generation &&
            static_cast<uint32_t>(player_state_get_index()) == result.track &&
            g_cover_generation == result.generation && g_cover_track == result.track;
        if (!result.ok || !still_current || g_controls_cache_back_pixels == nullptr) {
            return;
        }
        cassette_view_promote_controls_buffer(result.generation, result.track);
        ESP_LOGI(TAG,
            "当前磁带压暗快照预存就绪：track=%lu bytes=%u compose=%lldus",
            static_cast<unsigned long>(result.track),
            static_cast<unsigned>(kControlsCacheBytes),
            static_cast<long long>(result.elapsed_us));
        return;
    }

    if (g_next_prefetch.serial != result.serial ||
        g_next_prefetch.generation != result.generation ||
        g_next_prefetch.track != result.track) {
        if (result.fallback_lease.slot_index != 0xFFU) {
            fallback_cover_image_release(&result.fallback_lease);
        }
        return;
    }
    if (!result.ok || g_next_controls_cache_pixels == nullptr || g_next_shell_rgb565 == nullptr) {
        if (result.fallback_lease.slot_index != 0xFFU) {
            fallback_cover_image_release(&result.fallback_lease);
        }
        cassette_view_cancel_next_prefetch("离屏合成失败");
        return;
    }

    if (result.no_artwork) {
        if (g_next_prefetch.promotion_fallback.slot_index != 0xFFU) {
            fallback_cover_image_release(&g_next_prefetch.promotion_fallback);
        }
        if (result.fallback_lease.slot_index != 0xFFU &&
            result.fallback_lease.rgb565 != nullptr) {
            // 任务中的 fallback lease 直接转交给 next，保持解码后的缺省封面常驻到切歌。
            g_next_prefetch.promotion_fallback = result.fallback_lease;
            result.fallback_lease = {};
        } else {
            ESP_LOGW(TAG,
                "下一首缺省封面JPG不可用：track=%lu，预存黑标签+粉色磁带快照",
                static_cast<unsigned long>(result.track));
        }
    } else if (result.fallback_lease.slot_index != 0xFFU) {
        fallback_cover_image_release(&result.fallback_lease);
    }

    memcpy(g_next_prefetch.tint_lut, result.tint_lut, sizeof(result.tint_lut));
    g_next_prefetch.dynamic_tint = result.dynamic_tint;
    g_next_prefetch.no_artwork = result.no_artwork;
    g_next_prefetch.state = CassetteNextPrefetchState::Ready;
    ESP_LOGI(TAG,
        "下一首磁带预缓存就绪：track=%lu snapshot=%uB shell=%uB compose=%lldus mode=%s",
        static_cast<unsigned long>(result.track),
        static_cast<unsigned>(kControlsCacheBytes),
        static_cast<unsigned>(kPrefetchShellBytes),
        static_cast<long long>(result.elapsed_us),
        result.no_artwork ? "缺省封面+粉色" :
            (result.dynamic_tint ? "动态配色" : "原装粉色"));
}

static bool cassette_view_wait_cache_idle_for_trim()
{
    if (!g_cache_build_busy) return true;
    if (g_cache_result_queue == nullptr) return false;

    const TickType_t timeout = pdMS_TO_TICKS(kCassetteTrimWaitMs);
    const TickType_t started = xTaskGetTickCount();
    while (g_cache_build_busy) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout) return false;
        TickType_t wait = timeout - elapsed;
        const TickType_t slice = pdMS_TO_TICKS(20);
        if (wait > slice) wait = slice;

        CassetteCacheBuildResult result = {};
        if (xQueueReceive(g_cache_result_queue, &result, wait) != pdTRUE) continue;
        g_cache_build_busy = false;
        if (result.fallback_lease.slot_index != 0xFFU) {
            fallback_cover_image_release(&result.fallback_lease);
        }
    }
    return true;
}

static void cassette_view_free_psram(uint8_t **buffer)
{
    if (buffer == nullptr || *buffer == nullptr) return;
    heap_caps_free(*buffer);
    *buffer = nullptr;
}

static bool cassette_view_trim_memory()
{
    if (g_active) return false;
    if (!cassette_view_wait_cache_idle_for_trim()) {
        ESP_LOGW(TAG, "磁带PSRAM回收延后：后台缓存任务1秒内未退出，保留缓存避免UAF");
        return false;
    }

    const size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // 所有相关 LVGL 对象此时均已隐藏。descriptor 是静态对象，先解除数据引用，
    // 再释放像素，确保下一次进入磁带时只能经过 prepare/bind 重新建立来源。
    g_controls_cache_dsc = {};
    g_shell_dsc = {};
    g_big_reel_dsc = {};
    g_small_roller_dsc = {};
    g_tape_amount_dsc = {};

    cassette_view_free_psram(&g_controls_cache_pixels);
    cassette_view_free_psram(&g_controls_cache_back_pixels);
    cassette_view_free_psram(&g_next_controls_cache_pixels);
    cassette_view_free_psram(&g_next_shell_rgb565);

    cassette_view_free_psram(&g_shell_pixels);
    cassette_view_free_psram(&g_shell_base_rgb565);
    cassette_view_free_psram(&g_shell_tint_work_rgb565);
    cassette_view_free_psram(&g_shell_tint_luma);

    cassette_view_free_psram(&g_big_reel_pixels);
    cassette_view_free_psram(&g_small_roller_pixels);
    cassette_view_free_psram(&g_small_roller_base_rgb565);
    cassette_view_free_psram(&g_small_roller_tint_luma);
    cassette_view_free_psram(&g_tape_amount_pixels);

    g_controls_cache_ready = false;
    g_controls_cache_dirty = true;
    g_controls_cache_generation = 0U;
    g_controls_cache_track = UINT32_MAX;
    g_shell_tintable_pixels = 0U;
    g_shell_tint_generation = 0U;
    g_shell_tint_track = UINT32_MAX;
    g_small_roller_tintable_pixels = 0U;
    g_mechanics_ready = false;
    g_last_tape_shift = INT16_MIN;
    g_last_tape_path_step = -1;
    g_last_big_reel_frame[0] = 0xFFU;
    g_last_big_reel_frame[1] = 0xFFU;
    g_last_small_roller_frame = 0xFFU;
    g_last_glint_snapshot = 0xFFU;

    const size_t after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "磁带PSRAM已回收：reclaimed=%uB free=%u->%uB",
        static_cast<unsigned>(after >= before ? after - before : 0U),
        static_cast<unsigned>(before), static_cast<unsigned>(after));
    return true;
}

static bool cassette_view_schedule_current_cache()
{
    if (g_cache_build_busy || !g_active ||
        !cassette_view_current_visual_ready() ||
        !cassette_view_ensure_cache_buffer(
            &g_controls_cache_back_pixels, kControlsCacheBytes, "磁带控件后台交换缓存")) {
        return false;
    }
    // 当前快照始终优先于 next 预取，但不销毁已经准备好的 next。
    // 后台任务单实例串行执行，因此 current/next 可以安全长期各保留一张快照。
    const uint32_t generation = media_catalog_v2_generation();
    const uint32_t track = static_cast<uint32_t>(player_state_get_index());
    CassetteCacheBuildJob job = {};
    job.role = CassetteCacheBuildRole::Current;
    job.generation = generation;
    job.track = track;
    job.cover_y_offset_px = g_cover_y_offset_px;
    // 永远写 back buffer。即使 Controls 正在显示上一首 PSRAM 图，也不会边合成边改坏前台图。
    job.controls_output = g_controls_cache_back_pixels;
    job.no_artwork = g_cover_is_no_artwork_fallback;
    DeviceSettingsSnapshot settings = {};
    job.dynamic_tint = device_settings_get_snapshot(&settings) &&
        settings.cassette_dynamic_tint_enabled && !job.no_artwork;

    // 无封面 current 也让 Core1 任务自行 acquire fallback：命中已解码缓存时是快路径，
    // 缺省 JPG 缺失时则直接合成黑标签+粉色磁带，不在 UI tick 反复读 SD。
    if (!job.no_artwork && !cover_surface_cache_acquire_normal(track, &job.cover_lease)) {
        return false;
    }

    if (!cassette_view_submit_cache_job(&job)) {
        if (job.cover_lease.slot_index != 0xFFU) cover_surface_cache_release(&job.cover_lease);
        return false;
    }
    return true;
}

static bool cassette_view_schedule_next_build()
{
    if (g_cache_build_busy || g_next_prefetch.track == UINT32_MAX ||
        !cassette_view_ensure_cache_buffer(
            &g_next_controls_cache_pixels, kControlsCacheBytes, "下一首压暗磁带快照") ||
        !cassette_view_ensure_cache_buffer(
            &g_next_shell_rgb565, kPrefetchShellBytes, "下一首磁带壳缓存")) {
        return false;
    }

    if (!g_next_prefetch.no_artwork &&
        g_next_prefetch.promotion_cover.slot_index == 0xFFU) {
        return false;
    }

    CassetteCacheBuildJob job = {};
    job.role = CassetteCacheBuildRole::Next;
    job.generation = g_next_prefetch.generation;
    job.track = g_next_prefetch.track;
    job.cover_y_offset_px = 0;
    job.controls_output = g_next_controls_cache_pixels;
    job.shell_output = g_next_shell_rgb565;
    job.no_artwork = g_next_prefetch.no_artwork;
    DeviceSettingsSnapshot settings = {};
    job.dynamic_tint = device_settings_get_snapshot(&settings) &&
        settings.cassette_dynamic_tint_enabled && !job.no_artwork;

    if (!job.no_artwork &&
        !cover_surface_cache_acquire_normal(g_next_prefetch.track, &job.cover_lease)) {
        return false;
    }

    if (!cassette_view_submit_cache_job(&job)) {
        if (job.cover_lease.slot_index != 0xFFU) {
            cover_surface_cache_release(&job.cover_lease);
        }
        return false;
    }
    return true;
}

static bool cassette_view_current_lyrics_settled()
{
    if (!lyrics_service_is_ready() || !player_state_is_ready() || media_library_get_count() == 0U) {
        return true;
    }

    const uint32_t track = static_cast<uint32_t>(player_state_get_index());
    LyricsWindowSnapshot window = {};
    if (!lyrics_service_get_window(track, 0U, &window)) {
        return true;
    }

    // 下一首封面只是后台优化。当前曲歌词尚未完成读取/解析时先让出 TF 与 Core 1，
    // 等歌词进入终态后再启动下一首预缓存；当前曲封面加载不受这里限制。
    if (window.track_index != track) {
        return false;
    }
    return window.state != LyricsLoadState::Idle && window.state != LyricsLoadState::Loading;
}

static void cassette_view_service_next_prefetch()
{
    if (!g_active || g_cache_build_busy ||
        !player_state_is_ready() || media_library_get_count() == 0U) {
        return;
    }

    uint32_t next_track = UINT32_MAX;
    // 与手动切歌/EOF 共用 PlayerControl 串行锁；锁正忙时本轮直接跳过，
    // 不把一次瞬时锁竞争误判成“下一首变化”而销毁已经准备好的缓存。
    if (!player_control_peek_next_track(&next_track)) return;
    if (next_track == UINT32_MAX ||
        next_track == static_cast<uint32_t>(player_state_get_index())) {
        if (g_next_prefetch.state != CassetteNextPrefetchState::Idle) {
            cassette_view_cancel_next_prefetch("下一首不可预取");
        }
        return;
    }
    const uint32_t generation = media_catalog_v2_generation();

    if (g_next_prefetch.state != CassetteNextPrefetchState::Idle &&
        (g_next_prefetch.track != next_track || g_next_prefetch.generation != generation)) {
        cassette_view_cancel_next_prefetch("播放队列下一首变化");
    }

    if (g_next_prefetch.state == CassetteNextPrefetchState::Idle) {
        if (!cassette_view_current_lyrics_settled()) {
            return;
        }
        g_next_prefetch.generation = generation;
        g_next_prefetch.track = next_track;
        MediaArtworkViewV2 artwork = {};
        if (!media_library_get_artwork_view(next_track, &artwork)) {
            // 缺省封面也进入完整 next 预热：Core1 提前读/解 460x460 JPG，
            // 同时生成粉色磁带壳和压暗静态磁带，真正切歌时只做资源提升。
            g_next_prefetch.no_artwork = true;
            g_next_prefetch.state = CassetteNextPrefetchState::WaitingSurface;
            ESP_LOGI(TAG, "下一首缺省封面预取开始：track=%lu",
                static_cast<unsigned long>(next_track));
            (void)cassette_view_schedule_next_build();
            return;
        }

        CoverSurfaceLease promotion = {};
        if (cover_surface_cache_acquire_normal(next_track, &promotion)) {
            g_next_prefetch.promotion_cover = promotion;
            g_next_prefetch.state = CassetteNextPrefetchState::WaitingSurface;
            (void)cassette_view_schedule_next_build();
            return;
        }

        if (artwork_loader_request_track(next_track, &g_next_prefetch.artwork_request_id)) {
            g_next_prefetch.state = CassetteNextPrefetchState::WaitingArtwork;
            ESP_LOGI(TAG, "下一首封面预取开始：track=%lu request=%lu",
                static_cast<unsigned long>(next_track),
                static_cast<unsigned long>(g_next_prefetch.artwork_request_id));
        }
        return;
    }

    if (g_next_prefetch.state == CassetteNextPrefetchState::WaitingArtwork) {
        ArtworkCacheLease compressed = {};
        if (!artwork_loader_acquire_cached(next_track, &compressed)) return;
        artwork_loader_release_cached(&compressed);
        if (cover_surface_cache_request_track(next_track, &g_next_prefetch.surface_request_id)) {
            g_next_prefetch.state = CassetteNextPrefetchState::WaitingSurface;
        }
        return;
    }

    if (g_next_prefetch.state == CassetteNextPrefetchState::WaitingSurface) {
        if (!g_next_prefetch.no_artwork &&
            g_next_prefetch.promotion_cover.slot_index == 0xFFU) {
            CoverSurfaceLease promotion = {};
            if (!cover_surface_cache_acquire_normal(next_track, &promotion)) return;
            g_next_prefetch.promotion_cover = promotion;
        }
        (void)cassette_view_schedule_next_build();
    }
}

static void cassette_view_service_cache_pipeline()
{
    cassette_view_handle_cache_result();
    if (!g_active || g_root == nullptr || !cassette_view_current_visual_ready()) {
        return;
    }
    if (!g_present_deferred && lv_obj_has_flag(g_root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    // current 压暗快照属于磁带稳态资源：当前视觉一 ready 就后台预生成，
    // 因此用户第一次展开控件也能直接显示，不再等待现场合成。
    if (!cassette_view_controls_cache_matches_current()) {
        (void)cassette_view_schedule_current_cache();
        return;
    }
    // current 完整以后再准备 next：next normal/fallback + 找色 + 变色壳 + 压暗快照。
    cassette_view_service_next_prefetch();
}

static void cassette_view_cancel_shell_tint_job()
{
    g_shell_tint_job.in_progress = false;
    g_shell_tint_job.generation = 0U;
    g_shell_tint_job.track = UINT32_MAX;
    g_shell_tint_job.current_index = 0U;
    g_shell_tint_job.started_us = 0LL;
    if (g_shell_tint_timer != nullptr) lv_timer_pause(g_shell_tint_timer);
}

static void cassette_view_restore_shell_default()
{
    cassette_view_cancel_shell_tint_job();
    if (g_shell_pixels == nullptr || g_shell_base_rgb565 == nullptr) return;
    const size_t pixel_count = static_cast<size_t>(kCassetteWidth) * kCassetteHeight;
    const size_t rgb_bytes = pixel_count * 2U;
    memcpy(g_shell_pixels, g_shell_base_rgb565, rgb_bytes);
    g_shell_tint_generation = 0U;
    g_shell_tint_track = UINT32_MAX;
    cassette_view_mark_controls_cache_dirty();
    if (g_shell_image != nullptr) lv_obj_invalidate(g_shell_image);
}

static void cassette_view_tick_shell_tint_chunk()
{
    if (!g_shell_tint_job.in_progress) return;
    if (g_shell_tint_work_rgb565 == nullptr || g_shell_tint_luma == nullptr ||
        g_shell_pixels == nullptr) {
        cassette_view_cancel_shell_tint_job();
        return;
    }

    const uint32_t total_pixels = static_cast<uint32_t>(kCassetteWidth) *
        static_cast<uint32_t>(kCassetteHeight);
    uint32_t &index = g_shell_tint_job.current_index;
    if (index >= total_pixels) {
        cassette_view_cancel_shell_tint_job();
        return;
    }

    const uint32_t end = index + kShellTintChunkSize;
    const uint32_t stop = end < total_pixels ? end : total_pixels;
    for (; index < stop; ++index) {
        const uint8_t mapped_luma = g_shell_tint_luma[index];
        if (mapped_luma == 0U) continue;
        cassette_view_store_rgb565(
            g_shell_tint_work_rgb565 + index * 2U,
            g_shell_tint_job.tint_lut[mapped_luma - 1U]);
    }

    if (index < total_pixels) return;

    const uint32_t completed_generation = g_shell_tint_job.generation;
    const uint32_t completed_track = g_shell_tint_job.track;
    const int64_t total_cost_us = g_shell_tint_job.started_us > 0LL
        ? esp_timer_get_time() - g_shell_tint_job.started_us : 0LL;

    // TintJob 可能在 100ms 封面轮询发现切歌之前先跑完。提交前再核对一次真实曲目，
    // 过期结果只丢弃，绝不能把上一首的新颜色覆盖到当前歌曲。
    const bool still_current = player_state_is_ready() && media_library_get_count() > 0U &&
        static_cast<uint32_t>(player_state_get_index()) == completed_track &&
        media_catalog_v2_generation() == completed_generation;
    if (!still_current) {
        if (g_pending_cover_valid && g_pending_cover_generation == completed_generation &&
            g_pending_cover_track == completed_track) {
            cassette_view_release_pending_cover();
        }
        cassette_view_cancel_shell_tint_job();
        ESP_LOGI(TAG, "磁带壳动态配色丢弃过期结果：track=%lu",
            static_cast<unsigned long>(completed_track));
        return;
    }

    // 壳体、滚轮和 pending 封面都在同一个 LVGL timer 回调内提交。
    // LVGL 只会在回调结束后重绘，因此用户看到的是一整套新视觉，不会先出封面再补壳体颜色。
    const size_t rgb_bytes = static_cast<size_t>(total_pixels) * 2U;
    memcpy(g_shell_pixels, g_shell_tint_work_rgb565, rgb_bytes);
    cassette_view_apply_small_roller_tint(g_shell_tint_job.tint_lut);
    g_shell_tint_generation = completed_generation;
    g_shell_tint_track = completed_track;
    cassette_view_mark_controls_cache_dirty();
    const bool cover_committed = cassette_view_commit_pending_cover(
        completed_generation, completed_track);
    cassette_view_cancel_shell_tint_job();

    if (cover_committed && g_root != nullptr) {
        lv_obj_invalidate(g_root);
    } else if (g_shell_image != nullptr) {
        lv_obj_invalidate(g_shell_image);
    }
    ESP_LOGI(TAG,
        "磁带壳动态配色完成：track=%lu shell=%lu roller=%lu elapsed=%lldus",
        static_cast<unsigned long>(completed_track),
        static_cast<unsigned long>(g_shell_tintable_pixels),
        static_cast<unsigned long>(g_small_roller_tintable_pixels),
        static_cast<long long>(total_cost_us));
}

static void cassette_view_shell_tint_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    cassette_view_tick_shell_tint_chunk();
}

static bool cassette_view_start_shell_tint_job(
    uint32_t generation, uint32_t track, const CoverSurfaceLease &cover)
{
    if (g_shell_pixels == nullptr || g_shell_base_rgb565 == nullptr ||
        g_shell_tint_work_rgb565 == nullptr || g_shell_tint_luma == nullptr) {
        return false;
    }
    if (g_shell_tint_generation == generation && g_shell_tint_track == track) return true;
    if (g_shell_tint_job.in_progress &&
        g_shell_tint_job.generation == generation && g_shell_tint_job.track == track) {
        return true;
    }

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

    // 新封面着色始终在离屏工作区完成。切歌期间继续保留上一首已经完整显示的配色，
    // 不先回原装粉色；只有新颜色全部生成后，才一次性提交壳体和小滚轮。
    // 如果此前还有未完成的旧任务，只取消计算，不能改动当前显示缓冲。
    cassette_view_cancel_shell_tint_job();

    g_shell_tint_job.in_progress = true;
    g_shell_tint_job.generation = generation;
    g_shell_tint_job.track = track;
    g_shell_tint_job.current_index = 0U;
    g_shell_tint_job.started_us = esp_timer_get_time();
    memcpy(g_shell_tint_job.tint_lut, tint_lut, sizeof(tint_lut));

    if (g_shell_tint_timer != nullptr) {
        lv_timer_reset(g_shell_tint_timer);
        lv_timer_resume(g_shell_tint_timer);
    } else {
        // 初始化阶段若 timer 创建失败则同步推进，保证功能仍可退化工作。
        while (g_shell_tint_job.in_progress) cassette_view_tick_shell_tint_chunk();
    }

    ESP_LOGI(TAG,
        "磁带壳动态配色开始：track=%lu mode=%s samples=%lu source_hue=%u target_hue=%u rgb=#%02X%02X%02X",
        static_cast<unsigned long>(track),
        tint_mode,
        static_cast<unsigned long>(samples),
        static_cast<unsigned>(source_hue),
        static_cast<unsigned>(target_hue),
        static_cast<unsigned>(target.r),
        static_cast<unsigned>(target.g),
        static_cast<unsigned>(target.b));
    return true;
}

static void cassette_view_sync_tint_setting()
{
    DeviceSettingsSnapshot settings = {};
    const bool dynamic = device_settings_get_snapshot(&settings) &&
        settings.cassette_dynamic_tint_enabled;
    const bool mode_changed = !g_tint_setting_initialized ||
        dynamic != g_tint_setting_dynamic;
    g_tint_setting_initialized = true;
    g_tint_setting_dynamic = dynamic;
    if (mode_changed && g_next_prefetch.state != CassetteNextPrefetchState::Idle) {
        cassette_view_cancel_next_prefetch("磁带配色设置变化");
    }

    if (!dynamic) {
        if (g_shell_tint_job.in_progress) cassette_view_cancel_shell_tint_job();
        if (mode_changed || g_shell_tint_track != UINT32_MAX) {
            cassette_view_restore_shell_default();
            cassette_view_restore_small_roller_default();
            ESP_LOGI(TAG, "磁带配色：原装粉色");
        }
        // 若切换设置时恰好有尚未提交的新封面，粉色模式不需要等待 TintJob，
        // 直接把新封面与已经恢复的粉色壳体作为一套视觉提交。
        if (g_pending_cover_valid) {
            (void)cassette_view_commit_pending_cover(
                g_pending_cover_generation, g_pending_cover_track);
        }
        return;
    }

    // Catalog 已明确当前歌曲没有封面时，专用 fallback 已经把标签和粉色壳体一起提交。
    if (g_cover_is_no_artwork_fallback) {
        return;
    }

    if (!player_state_is_ready() || media_library_get_count() == 0U) return;
    const uint32_t current_track = static_cast<uint32_t>(player_state_get_index());
    const uint32_t current_generation = media_catalog_v2_generation();

    // 新封面已经 acquire 但尚未上屏：只允许它自己的 TintJob 继续跑。
    // 显示层此时仍保持上一首完整封面+壳体，不能因为 g_cover_track 还是上一首而误取消任务。
    if (g_pending_cover_valid && g_pending_cover_track == current_track &&
        g_pending_cover_generation == current_generation) {
        if (!g_shell_tint_job.in_progress ||
            g_shell_tint_job.generation != current_generation ||
            g_shell_tint_job.track != current_track) {
            (void)cassette_view_start_shell_tint_job(
                current_generation, current_track, g_pending_cover_lease);
        }
        return;
    }
    if (g_pending_cover_valid) {
        // 连续快速切歌时，新曲 Surface 可能还没 ready。旧 pending lease 必须立即释放，
        // 但屏幕上的上一套已提交视觉继续保留，直到真正的新 Surface 到达。
        if (g_shell_tint_job.in_progress) cassette_view_cancel_shell_tint_job();
        cassette_view_release_pending_cover();
    }

    // 新歌曲 Surface 尚未 ready 时继续保留上一首完整视觉；只丢弃真正过期的后台任务。
    if (g_cover_track != current_track || g_cover_generation != current_generation) {
        if (g_shell_tint_job.in_progress) cassette_view_cancel_shell_tint_job();
        return;
    }

    if (g_cover_lease.slot_index == 0xFFU || g_cover_lease.normal_rgb565 == nullptr) {
        return;
    }
    if (g_shell_tint_generation == g_cover_generation &&
        g_shell_tint_track == g_cover_track) {
        return;
    }
    if (g_shell_tint_job.in_progress &&
        g_shell_tint_job.generation == g_cover_generation &&
        g_shell_tint_job.track == g_cover_track) {
        return;
    }

    (void)cassette_view_start_shell_tint_job(
        g_cover_generation, g_cover_track, g_cover_lease);
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

    uint8_t *tint_work_rgb565 = static_cast<uint8_t *>(heap_caps_malloc(
        rgb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (tint_work_rgb565 == nullptr) {
        // 离屏缓存属于性能优化资源；申请失败时保留原装粉色，不让磁带页面整体失败。
        ESP_LOGW(TAG, "磁带壳离屏着色缓存不足：%uB；动态配色将降级为原装粉色",
            static_cast<unsigned>(rgb_bytes));
    }

    uint8_t *tint_luma = static_cast<uint8_t *>(heap_caps_malloc(
        pixel_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (tint_luma == nullptr) {
        ESP_LOGE(TAG, "磁带壳着色Mask PSRAM 不足：%uB", static_cast<unsigned>(pixel_count));
        if (tint_work_rgb565 != nullptr) heap_caps_free(tint_work_rgb565);
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
    if (tint_work_rgb565 != nullptr) memcpy(tint_work_rgb565, base_rgb565, rgb_bytes);
    heap_caps_free(rgba);

    g_shell_pixels = native;
    g_shell_base_rgb565 = base_rgb565;
    g_shell_tint_work_rgb565 = tint_work_rgb565;
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

    ESP_LOGI(TAG, "磁带壳已准备：PNG=%uB RGB565A8=%uB 原色=%uB 离屏=%uB Mask=%uB tintable=%lu stride=%u PSRAM",
        static_cast<unsigned>(png_size),
        static_cast<unsigned>(native_bytes),
        static_cast<unsigned>(rgb_bytes),
        static_cast<unsigned>(tint_work_rgb565 != nullptr ? rgb_bytes : 0U),
        static_cast<unsigned>(pixel_count),
        static_cast<unsigned long>(g_shell_tintable_pixels),
        static_cast<unsigned>(g_shell_dsc.header.stride));
    return true;
}

static bool cassette_view_try_promote_next_visual(uint32_t generation, uint32_t track)
{
    if (g_next_prefetch.state != CassetteNextPrefetchState::Ready ||
        g_next_prefetch.generation != generation || g_next_prefetch.track != track ||
        g_next_controls_cache_pixels == nullptr || g_next_shell_rgb565 == nullptr) {
        return false;
    }
    if (!g_next_prefetch.no_artwork &&
        (g_next_prefetch.promotion_cover.slot_index == 0xFFU ||
         g_next_prefetch.promotion_cover.normal_rgb565 == nullptr)) {
        return false;
    }

    DeviceSettingsSnapshot settings = {};
    const bool dynamic_now = device_settings_get_snapshot(&settings) &&
        settings.cassette_dynamic_tint_enabled;
    const bool expected_dynamic = dynamic_now && !g_next_prefetch.no_artwork;
    if (expected_dynamic != g_next_prefetch.dynamic_tint) {
        cassette_view_cancel_next_prefetch("配色模式已变化");
        return false;
    }

    cassette_view_cancel_shell_tint_job();
    cassette_view_release_pending_cover();

    CoverSurfaceLease old_cover = g_cover_lease;
    FallbackCoverImageLease old_fallback = g_fallback_cover_lease;
    g_cover_lease = {};
    g_fallback_cover_lease = {};
    g_cover_generation = generation;
    g_cover_track = track;
    g_cover_y_offset_px = 0;
    g_cover_is_no_artwork_fallback = g_next_prefetch.no_artwork;

    const uint8_t *cover_pixels = nullptr;
    uint16_t cover_width = 0U;
    uint16_t cover_height = 0U;
    size_t cover_size = 0U;
    if (g_next_prefetch.no_artwork) {
        g_fallback_cover_lease = g_next_prefetch.promotion_fallback;
        g_next_prefetch.promotion_fallback = {};
        if (g_fallback_cover_lease.slot_index != 0xFFU &&
            g_fallback_cover_lease.rgb565 != nullptr) {
            cover_pixels = g_fallback_cover_lease.rgb565;
            cover_width = g_fallback_cover_lease.width;
            cover_height = g_fallback_cover_lease.height;
            cover_size = g_fallback_cover_lease.data_size;
        }
    } else {
        g_cover_lease = g_next_prefetch.promotion_cover;
        g_next_prefetch.promotion_cover = {};
        cover_pixels = g_cover_lease.normal_rgb565;
        cover_width = g_cover_lease.width;
        cover_height = g_cover_lease.height;
        cover_size = g_cover_lease.data_size;
    }

    if (cover_pixels != nullptr && cover_width > 0U && cover_height > 0U) {
        cassette_view_init_rgb565_dsc(
            &g_cover_dsc, cover_pixels, cover_width, cover_height, cover_size);
        g_cover_scale_q8 = cassette_view_cover_scale_for_size(cover_width, cover_height);
        lv_image_set_src(g_cover_image, &g_cover_dsc);
        lv_image_set_scale(g_cover_image, g_cover_scale_q8);
        lv_image_set_antialias(g_cover_image, false);
        cassette_view_apply_cover_position();
        lv_obj_remove_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);
    } else {
        g_cover_dsc = {};
        g_cover_scale_q8 = kLvImageScaleNone;
        lv_obj_add_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);
    }

    memcpy(g_shell_pixels, g_next_shell_rgb565, kPrefetchShellBytes);
    if (expected_dynamic) {
        cassette_view_apply_small_roller_tint(g_next_prefetch.tint_lut);
        g_shell_tint_generation = generation;
        g_shell_tint_track = track;
    } else {
        cassette_view_restore_small_roller_default();
        g_shell_tint_generation = 0U;
        g_shell_tint_track = UINT32_MAX;
    }
    if (g_shell_image != nullptr) lv_obj_invalidate(g_shell_image);

    // next 的压暗静态磁带已经完整生成，切歌只交换 current/next 指针。
    // 控件若正在显示，背景在同一个 LVGL 回调内直接换成新歌，不闪动态中间态。
    if (!cassette_view_promote_next_controls_buffer(generation, track)) {
        return false;
    }

    if (old_cover.slot_index != 0xFFU) cover_surface_cache_release(&old_cover);
    if (old_fallback.slot_index != 0xFFU) fallback_cover_image_release(&old_fallback);
    fallback_cover_image_discard_unpinned();
    if (!g_cover_is_no_artwork_fallback) {
        cover_surface_cache_retain_track(track);
    }

    const uint32_t promoted_track = g_next_prefetch.track;
    const bool promoted_fallback = g_next_prefetch.no_artwork;
    g_next_prefetch = {};
    lv_obj_invalidate(g_root);
    ESP_LOGI(TAG,
        "下一首磁带预缓存命中：track=%lu %s+磁带壳+压暗快照直接提升",
        static_cast<unsigned long>(promoted_track),
        promoted_fallback ? "缺省封面" : "原始封面");
    return true;
}

static bool cassette_view_bind_current_cover()
{
    if (!player_state_is_ready() || media_library_get_count() == 0U || g_cover_image == nullptr) {
        return false;
    }

    const uint32_t generation = media_catalog_v2_generation();
    const uint32_t track = static_cast<uint32_t>(player_state_get_index());

    // next 预缓存本来就应该与“当前正在播放/显示的歌曲”不同。
    // 只有播放器目标已经离开屏幕当前可见视觉，并且现有 next 也不是这个新目标时，
    // 才说明用户手动选择了另一个未预热曲目，需要取消旧 next。稳定播放 A、后台预热 B
    // 时绝不能因为 B != A 而在每个 UI tick 反复取消/重启 Artwork 请求。
    const bool target_changed_from_visible =
        g_cover_generation != generation || g_cover_track != track;
    if (target_changed_from_visible &&
        g_next_prefetch.state != CassetteNextPrefetchState::Idle &&
        g_next_prefetch.track != track) {
        cassette_view_cancel_next_prefetch("当前目标未命中预缓存");
    }

    // Round 23：当前曲若正好是上一首播放期间完整预热的 next，直接提升整套视觉。
    if (cassette_view_try_promote_next_visual(generation, track)) {
        return true;
    }

    if (g_cover_is_no_artwork_fallback &&
        g_cover_generation == generation && g_cover_track == track) {
        return true;
    }
    if (g_cover_lease.slot_index != 0xFFU &&
        g_cover_generation == generation && g_cover_track == track) {
        return true;
    }
    if (g_pending_cover_valid && g_pending_cover_generation == generation &&
        g_pending_cover_track == track) {
        return true;
    }

    // “没有封面”与“封面仍在后台准备”必须分开处理：
    // 前者提交磁带专用 fallback + 粉色壳体；后者继续保留上一套完整视觉。
    MediaArtworkViewV2 artwork = {};
    if (!media_library_get_artwork_view(track, &artwork)) {
        return cassette_view_bind_no_artwork_label(generation, track);
    }

    CoverSurfaceLease next = {};
    if (!cover_surface_cache_acquire_normal(track, &next) || next.normal_rgb565 == nullptr ||
        next.width == 0U || next.height == 0U || next.data_size == 0U) {
        return false;
    }

    // 新 Surface 先进入 pending，不绑定到 g_cover_image。这样切歌时当前屏幕继续保持上一首
    // 的完整封面+壳体，直到新壳体颜色也准备完成。
    cassette_view_cancel_shell_tint_job();
    cassette_view_release_pending_cover();
    g_pending_cover_lease = next;
    g_pending_cover_generation = generation;
    g_pending_cover_track = track;
    g_pending_cover_y_offset_px = g_cover_track == track ? g_cover_y_offset_px : 0;
    g_pending_cover_valid = true;

    DeviceSettingsSnapshot settings = {};
    const bool dynamic = device_settings_get_snapshot(&settings) &&
        settings.cassette_dynamic_tint_enabled;
    if (!dynamic) {
        // 原装粉色模式不需要等待 TintJob：先恢复粉色，再在同一回调提交新封面。
        cassette_view_restore_shell_default();
        cassette_view_restore_small_roller_default();
        (void)cassette_view_commit_pending_cover(generation, track);
    } else if (g_shell_tint_generation == generation && g_shell_tint_track == track) {
        // 从封面视图再次进入同一首歌时，壳体显示缓冲仍保留上次完整配色，直接绑定封面即可。
        // 这样既不重复计算，也不会在首次切换画面里闪出粉色壳体。
        (void)cassette_view_commit_pending_cover(generation, track);
    } else if (!cassette_view_start_shell_tint_job(generation, track, g_pending_cover_lease)) {
        // PSRAM/着色缓存不可用时不能让首次切换永远等不到 ready；降级为完整粉色+新封面。
        cassette_view_restore_shell_default();
        cassette_view_restore_small_roller_default();
        (void)cassette_view_commit_pending_cover(generation, track);
        g_shell_tint_generation = generation;
        g_shell_tint_track = track;
        ESP_LOGW(TAG, "磁带动态配色不可用：track=%lu，降级为完整粉色视觉",
            static_cast<unsigned long>(track));
    }

    ESP_LOGI(TAG,
        "磁带封面已准备：track=%lu source=%ux%u dynamic=%d；等待与壳体同帧提交",
        static_cast<unsigned long>(track),
        static_cast<unsigned>(next.width),
        static_cast<unsigned>(next.height),
        dynamic ? 1 : 0);
    return true;
}

static bool cassette_view_current_visual_ready()
{
    if (!player_state_is_ready() || media_library_get_count() == 0U) return false;
    const uint32_t generation = media_catalog_v2_generation();
    const uint32_t track = static_cast<uint32_t>(player_state_get_index());
    if (g_cover_generation != generation || g_cover_track != track) return false;
    if (g_cover_is_no_artwork_fallback) return true;
    if (g_cover_lease.slot_index == 0xFFU || g_cover_lease.normal_rgb565 == nullptr) return false;

    DeviceSettingsSnapshot settings = {};
    const bool dynamic = device_settings_get_snapshot(&settings) &&
        settings.cassette_dynamic_tint_enabled;
    if (!dynamic) return true;
    return g_shell_tint_generation == generation && g_shell_tint_track == track;
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
    if (g_current_lyric_label != nullptr) {
        lv_label_set_long_mode(g_current_lyric_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(g_current_lyric_label, 2, 0);
    }
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
    g_tape_glints[2] = cassette_view_create_tape_glint(g_root, 4, 2);
    g_tape_glints[3] = cassette_view_create_tape_glint(g_root, 3, 2);
    if (g_tape_middle_line == nullptr || g_tape_glints[2] == nullptr ||
        g_tape_glints[3] == nullptr) {
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

    g_controls_cache_image = lv_image_create(g_root);
    if (g_controls_cache_image == nullptr) return ESP_ERR_NO_MEM;
    ui_common_lock_object(g_controls_cache_image);
    lv_obj_set_pos(g_controls_cache_image, 0, 0);
    lv_obj_add_flag(g_controls_cache_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_controls_cache_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_controls_cache_image, LV_OBJ_FLAG_SCROLLABLE);

    if (!cassette_view_start_cache_task()) {
        ESP_LOGW(TAG, "磁带预缓存任务创建失败：保留实时Backdrop兜底，不影响动态磁带");
    }

    g_mechanics_timer = lv_timer_create(
        cassette_view_mechanics_timer_cb, kMechanicsTimerPeriodMs, nullptr);
    if (g_mechanics_timer == nullptr) return ESP_ERR_NO_MEM;
    lv_timer_pause(g_mechanics_timer);

    g_shell_tint_timer = lv_timer_create(
        cassette_view_shell_tint_timer_cb, kShellTintTimerPeriodMs, nullptr);
    if (g_shell_tint_timer != nullptr) {
        lv_timer_pause(g_shell_tint_timer);
    } else {
        // timer 申请失败只影响分块调度；TintJob 会自动退化为一次性同步完成。
        ESP_LOGW(TAG, "磁带壳着色 timer 创建失败：动态配色将使用同步退化路径");
    }

    return ESP_OK;
}

static bool cassette_view_set_active_internal(bool active, bool preserve_next_prefetch)
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
        // 先消费后台 current/next 合成结果；若当前曲正好命中预热 next，
        // 本轮 bind 就能直接提升完整视觉，不先误启动一轮普通 TintJob。
        cassette_view_handle_cache_result();
        if (g_cover_lease.slot_index == 0xFFU && g_fallback_cover_lease.slot_index == 0xFFU) {
            (void)cassette_view_apply_transition_hold();
        } else {
            cassette_view_release_transition_hold();
        }
        (void)cassette_view_bind_current_cover();
        cassette_view_sync_tint_setting();
        g_active = true;
        g_last_mechanics_frame_us = esp_timer_get_time();
        g_last_tape_glint_us = g_last_mechanics_frame_us;

        // 封面→磁带首次切换可以要求 deferred present：磁带在隐藏状态完成新封面+壳体配色，
        // 由 player_home 在 ready 后再同一帧撤掉 Artwork 并显示 Cassette。
        if (g_present_deferred) {
            lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
            if (g_mechanics_timer != nullptr) lv_timer_pause(g_mechanics_timer);
        } else {
            lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
            if (g_mechanics_timer != nullptr) {
                if (mechanics_ok && !g_seek_frozen && !g_controls_visible && !g_launcher_suspended) {
                    lv_timer_resume(g_mechanics_timer);
                } else {
                    lv_timer_pause(g_mechanics_timer);
                }
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
    g_present_deferred = false;
    // 真正退出磁带视图时不长期 pin 下一首 CoverSurface，避免占用双槽封面缓存影响 Artwork 模式。
    // Launcher 仅临时隐藏场景时保留 next；关闭菜单后继续消费，不重复读盘/合成。
    if (!preserve_next_prefetch && g_next_prefetch.state != CassetteNextPrefetchState::Idle) {
        cassette_view_cancel_next_prefetch("磁带视图退出");
    }
    if (g_controls_cache_image != nullptr) {
        lv_obj_add_flag(g_controls_cache_image, LV_OBJ_FLAG_HIDDEN);
    }
    // Launcher 临时隐藏不代表配色设置发生了变化；保留已确认状态，
    // 否则恢复时 sync_tint_setting() 会把重新初始化误判为设置变化，再次取消 next。
    if (!preserve_next_prefetch) {
        g_tint_setting_initialized = false;
    }
    cassette_view_cancel_shell_tint_job();
    if (g_mechanics_timer != nullptr) lv_timer_pause(g_mechanics_timer);
    cassette_view_set_mechanics_visible(false);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    cassette_view_release_cover(preserve_next_prefetch);
    cassette_view_release_transition_hold();
    g_text_track = UINT32_MAX;
    g_lyrics_requested_track = UINT32_MAX;
    cassette_view_clear_lyrics();
    cassette_view_apply_aux_visibility();
    if (!preserve_next_prefetch) {
        (void)cassette_view_trim_memory();
    }
    return true;
}

bool cassette_view_set_active(bool active)
{
    return cassette_view_set_active_internal(active, false);
}

bool cassette_view_set_temporary_hidden(bool hidden)
{
    // Music 内部全屏页、Launcher 或轻量 APP 只是临时接管屏幕：隐藏磁带场景时保留下一首预缓存。
    // 真正进入高占用媒体 APP 或离开磁带模式仍走 cassette_view_set_active(false)，按原逻辑释放。
    return cassette_view_set_active_internal(!hidden, true);
}

bool cassette_view_prepare_track_transition_hold()
{
    cassette_view_release_transition_hold();
    if (!player_state_is_ready() || media_library_get_count() == 0U ||
        !cover_surface_cache_is_ready()) {
        return false;
    }

    const uint32_t track = static_cast<uint32_t>(player_state_get_index());
    CoverSurfaceLease lease = {};
    if (!cover_surface_cache_acquire_normal(track, &lease)) {
        ESP_LOGI(TAG, "磁带曲库交接未命中旧Surface：track=%lu",
            static_cast<unsigned long>(track));
        return false;
    }
    if (lease.normal_rgb565 == nullptr || lease.width == 0U || lease.height == 0U ||
        lease.data_size == 0U) {
        cover_surface_cache_release(&lease);
        return false;
    }

    g_transition_hold_lease = lease;
    ESP_LOGI(TAG, "磁带曲库交接pin旧封面：track=%lu revision=%lu",
        static_cast<unsigned long>(track),
        static_cast<unsigned long>(lease.slot_revision));
    return true;
}

void cassette_view_cancel_track_transition_hold()
{
    cassette_view_release_transition_hold();
}

bool cassette_view_prepare_deferred_active()
{
    g_present_deferred = true;
    if (!cassette_view_set_active(true)) {
        g_present_deferred = false;
        return false;
    }
    return true;
}

bool cassette_view_try_present_deferred()
{
    if (!g_active || g_root == nullptr || !g_present_deferred) return false;

    // Surface 可能比 TintJob 晚到；每次尝试提交前再消费一次当前封面状态。
    (void)cassette_view_bind_current_cover();
    cassette_view_sync_tint_setting();
    if (!cassette_view_current_visual_ready()) return false;

    // 如果切换磁带模式时 Controls 已经打开，则隐藏状态下把当前曲压暗缓存也准备完整。
    // 这样 Artwork/旧视觉会一直保持到“新封面+新壳体+新PSRAM控件背景”同时可交接，
    // 屏幕不会先出现一版实时 Alpha 压暗磁带，再被缓存版本二次替换。
    if (g_controls_visible && !cassette_view_controls_cache_matches_current()) {
        cassette_view_service_cache_pipeline();
        return false;
    }

    g_present_deferred = false;
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    g_last_mechanics_frame_us = esp_timer_get_time();
    g_last_tape_glint_us = g_last_mechanics_frame_us;
    if (g_mechanics_timer != nullptr) {
        if (g_mechanics_ready && !g_seek_frozen && !g_controls_visible && !g_launcher_suspended) {
            lv_timer_reset(g_mechanics_timer);
            lv_timer_resume(g_mechanics_timer);
        } else {
            lv_timer_pause(g_mechanics_timer);
        }
    }
    cassette_view_set_mechanics_visible(g_mechanics_ready);
    cassette_view_update_track_text();
    cassette_view_apply_aux_visibility();
    cassette_view_update_mini_lyrics();
    cassette_view_update_mechanics();
    // Controls 已打开时，上面已经等待压暗静态快照 ready；这里一次提交完整磁带控件背景。
    lv_obj_invalidate(g_root);
    return true;
}

bool cassette_view_is_present_ready()
{
    return g_active && cassette_view_current_visual_ready();
}

void cassette_view_update()
{
    if (!g_active || g_root == nullptr) return;
    // 预缓存结果必须优先于当前曲绑定消费；否则 A→B 的第一轮更新会看不到刚完成的 B，
    // 先走普通封面/Tint路径，白白增加一次切歌等待。
    cassette_view_handle_cache_result();
    (void)cassette_view_bind_current_cover();
    cassette_view_sync_tint_setting();
    cassette_view_update_track_text();
    cassette_view_update_mini_lyrics();
    cassette_view_service_cache_pipeline();
    // 机械层由独立 50ms timer 驱动；Controls/next 缓存由 Core1 低优先级任务离屏生成，
    // 这里仅消费结果和推进预取状态，不再同步 Snapshot 阻塞机械 timer。
}

static void cassette_view_revalidate_next_prefetch(const char *cancel_reason)
{
    if (!player_state_is_ready() || media_library_get_count() == 0U) {
        return;
    }

    uint32_t next_track = UINT32_MAX;
    // 播放模式/目录队列变化与手动切歌共用 PlayerControl 串行锁；锁忙时不破坏旧缓存，
    // 后续 cassette_view_update() 会再次复核。
    if (!player_control_peek_next_track(&next_track)) {
        return;
    }

    const uint32_t current_track = static_cast<uint32_t>(player_state_get_index());
    const uint32_t generation = media_catalog_v2_generation();
    const bool next_available = next_track != UINT32_MAX && next_track != current_track;

    if (g_next_prefetch.state != CassetteNextPrefetchState::Idle &&
        (!next_available || g_next_prefetch.track != next_track ||
         g_next_prefetch.generation != generation)) {
        cassette_view_cancel_next_prefetch(cancel_reason);
    }

    // 曲库/设置等全屏页覆盖主页时，磁带层不启动新的后台预取；
    // 但旧 next 会在队列变化当下立即失效，返回磁带后再按新 next 启动预缓存。
    if (g_active && next_available) {
        cassette_view_service_next_prefetch();
    }
}

void cassette_view_on_playback_mode_changed()
{
    cassette_view_revalidate_next_prefetch("播放模式下一首变化");
}

void cassette_view_on_playback_queue_changed()
{
    cassette_view_revalidate_next_prefetch("目录队列下一首变化");
}

void cassette_view_set_controls_visible(bool visible)
{
    if (g_controls_visible == visible) {
        cassette_view_apply_aux_visibility();
        if (visible && g_active && !cassette_view_controls_cache_matches_current()) {
            (void)cassette_view_schedule_current_cache();
        }
        return;
    }

    g_controls_visible = visible;
    if (!g_active) {
        cassette_view_apply_aux_visibility();
        return;
    }

    if (visible) {
        // current 快照在正常磁带播放期间已经预存。命中时直接显示；
        // 极端情况下还没准备完，则保持冻结画面并补做一次 current。
        if (cassette_view_controls_cache_matches_visible_visual() &&
            g_controls_cache_image != nullptr) {
            lv_obj_remove_flag(g_controls_cache_image, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(g_controls_cache_image);
        } else {
            (void)cassette_view_schedule_current_cache();
        }
    } else if (g_controls_cache_image != nullptr) {
        // 关闭控件只隐藏，不释放 current 快照；下一次展开直接复用。
        lv_obj_add_flag(g_controls_cache_image, LV_OBJ_FLAG_HIDDEN);
    }
    cassette_view_apply_aux_visibility();

    // Controls 打开时冻结真实机械层；静态快照 ready 前也保持冻结，
    // 不让用户看到“先动着压暗、随后再冻结”的两阶段画面。
    const int64_t now_us = esp_timer_get_time();
    g_last_mechanics_frame_us = now_us;
    g_last_tape_glint_us = now_us;

    const bool should_resume_mechanics =
        !visible && g_active && g_mechanics_ready && !g_seek_frozen && !g_launcher_suspended;
    if (g_mechanics_timer != nullptr && visible) {
        lv_timer_pause(g_mechanics_timer);
    }

    if (g_active && !visible) {
        // 控件关闭后先刷新封面/文字/歌词，再按当前真实播放进度同步机械层；
        // 时间基准已重置，不补跑控件显示期间漏掉的卷轴帧。
        cassette_view_update();
        cassette_view_update_mechanics();
    }

    if (g_mechanics_timer != nullptr && should_resume_mechanics) {
        lv_timer_reset(g_mechanics_timer);
        lv_timer_resume(g_mechanics_timer);
    }

    ESP_LOGI(TAG, "控件机械动画：%s cache=%s",
        visible ? "冻结" : "恢复",
        visible && cassette_view_controls_cache_active() ? "PSRAM直显" :
            (visible ? "准备中" : "预存保留"));
}

bool cassette_view_controls_cache_active()
{
    // Controls 已经显示的 PSRAM 图本身就是当前屏幕的稳定背景。
    // 即使 PlayerState 已切到新歌、甚至底层新封面+壳体已经提交，也要保持这张旧图，
    // 直到新歌 back buffer 完整生成后由 promote_controls_buffer() 一次换源。
    return g_active && g_controls_visible && g_controls_cache_ready &&
        g_controls_cache_image != nullptr &&
        !lv_obj_has_flag(g_controls_cache_image, LV_OBJ_FLAG_HIDDEN);
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
