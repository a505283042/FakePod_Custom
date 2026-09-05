#include "spectrum_view.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio_service.h"
#include "audio/audio_types.h"
#include "esp_log.h"
#include "app_diag_config.h"
#include "font/font_manager.h"
#include "gesture/gesture_router.h"
#include "media/library/media_catalog_v2.h"
#include "lyrics_service.h"
#include "player/player_playlist.h"
#include "system/screen_lock_simple.h"
#include "ui_common.h"

namespace
{
static const char *TAG = "频谱界面";

#if APP_DIAG_BOOT_VERBOSE
#define UI_PAGE_BOOT_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define UI_PAGE_BOOT_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif

#if APP_DIAG_UI_INTERACTION
#define UI_PAGE_INTERACTION_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define UI_PAGE_INTERACTION_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif

constexpr uint32_t SPECTRUM_FRAME_MS = 50U; // P1.5.2R.3.1：20 FPS，只消费最新16-band FFT Snapshot。
constexpr uint8_t SPECTRUM_BAR_COUNT = 16U; // FFT 数据 band 数，保持不变。
constexpr uint8_t SPECTRUM_VISUAL_BAR_COUNT = 24U; // P1.5.2R.4：绘制层插值为24根镜像音柱。
constexpr int16_t SPECTRUM_BAR_W = 9;
constexpr int16_t SPECTRUM_BAR_GAP = 5;
constexpr int16_t SPECTRUM_AREA_LEFT = 50;
constexpr int16_t SPECTRUM_AREA_TOP = 126;
constexpr int16_t SPECTRUM_AREA_W = 360;
constexpr int16_t SPECTRUM_AREA_H = 230;
constexpr int16_t SPECTRUM_BASELINE_GAP = 2;
constexpr int16_t SPECTRUM_MIN_H = 2;
constexpr int16_t SPECTRUM_MAX_H = (SPECTRUM_AREA_H / 2) - 8;
constexpr uint8_t SPECTRUM_REFLECTION_PERCENT = 55U; // P1.5.2R.4.1：下半只做短倒影。
constexpr int16_t SPECTRUM_PEAK_DOT_W = 5;
constexpr int16_t SPECTRUM_PEAK_DOT_H = 3;
constexpr int16_t SPECTRUM_PEAK_DOT_GAP = 4;
constexpr uint16_t SPECTRUM_PEAK_FALL_PX = 3U; // 20 FPS 下约 60px/s 的独立下落速度。

// P1.5.3.2R.35.2：频谱样式只接受真正轻点。
// 页面纵向切歌阈值较大，因此这里额外限制整次触摸生命周期最大位移；
// 任何明显拖动即使没有形成 Swipe，也不能在 RELEASE 后误触发样式切换。
constexpr int16_t SPECTRUM_STYLE_TAP_MAX_MOVE_PX = 10;

// P1.5.3.2R.35.2：仅调整“显示频谱”的频段视觉增益，不修改 FFT Snapshot，
// 更不会影响 PCM / I2S 音频输出。AudioSpectrumSnapshot 的 16 band 顺序为低频→高频。
// 低频保持约 1.00x，中频只轻微抬升，主要从中高频开始补偿，最高频达到 1.50x。
// 使用 Q8 定点倍率，避免 20 FPS UI 热路径引入浮点运算。
constexpr uint16_t SPECTRUM_BAND_GAIN_Q8[SPECTRUM_BAR_COUNT] = {
    256, 256, 256, 256, // 1.00, 1.00, 1.00, 1.00
    261, 266, 271, 279, // 1.02, 1.04, 1.06, 1.09
    289, 302, 317, 335, // 1.13, 1.18, 1.24, 1.31
    353, 366, 376, 384, // 1.38, 1.43, 1.47, 1.50
};

// P1.5.2R.4.4：频谱下方固定预留两行高度，只显示“当前这一句歌词”。
// 短句保持单行；需要两行时按实际 glyph 像素宽度寻找更均衡的断点，而不是贪心塞满第一行。
constexpr int16_t SPECTRUM_LYRIC_X = 22;
constexpr int16_t SPECTRUM_LYRIC_Y = 316;
constexpr int16_t SPECTRUM_LYRIC_W = 416;
constexpr int16_t SPECTRUM_LYRIC_H = 62;
constexpr int16_t SPECTRUM_LYRIC_LINE_SPACE = 2;
constexpr uint32_t SPECTRUM_LYRIC_POLL_FRAMES = 2U; // 50ms频谱timer下约100ms检查一次歌词行。
constexpr int32_t SPECTRUM_LYRIC_TEXT_MAX_W = SPECTRUM_LYRIC_W - 8;
constexpr size_t SPECTRUM_LYRIC_FORMATTED_BYTES = LYRICS_VIEW_TEXT_BYTES + 8U;

// P1.5.3.2R.6：SegmentedColumns 调整为第一/默认样式，HorizontalMirror 第二，NeonRidge 第三。
// SegmentedColumns 仍只消费现有16-band FFT Snapshot：每列由多个小矩形堆叠，
// 下方绘制更暗的短倒影；每列最少保留1格，Peak落点改为与主柱同宽的窄矩形。
enum class SpectrumStyle : uint8_t
{
    SegmentedColumns = 0,
    HorizontalMirror,
    NeonRidge,
    Count,
};

// Neon Ridge：单对象内将16-band FFT插值成40个连续点。
// 保持大面积纯黑留白，只绘制暗色尾迹 + 亮色主山脊 + 极少量峰值光点。
constexpr uint8_t SPECTRUM_RIDGE_POINT_COUNT = 40U;
constexpr int16_t SPECTRUM_RIDGE_X = 36;
constexpr int16_t SPECTRUM_RIDGE_Y = 116;
constexpr int16_t SPECTRUM_RIDGE_W = 388;
constexpr int16_t SPECTRUM_RIDGE_H = 190;
constexpr int16_t SPECTRUM_RIDGE_BASELINE_Y = 154;
constexpr int16_t SPECTRUM_RIDGE_MIN_H = 6;
constexpr int16_t SPECTRUM_RIDGE_MAX_H = 118;
constexpr int16_t SPECTRUM_RIDGE_TRAIL_OFFSET_Y = 7;
constexpr uint8_t SPECTRUM_RIDGE_TRAIL_PERCENT = 28U;
constexpr int16_t SPECTRUM_RIDGE_MAIN_W = 3;
constexpr int16_t SPECTRUM_RIDGE_TRAIL_W = 3;
constexpr int16_t SPECTRUM_RIDGE_PEAK_DOT = 3;
// P1.5.3.2：在每个山脊采样点位置绘制一根细竖线，从基线连到主山脊。
// 竖线只做辅助结构，颜色压暗，避免重新变成密集传统均衡器。
constexpr uint8_t SPECTRUM_RIDGE_STEM_STEP = 1U; // 加密：每个山脊采样点都画一根竖线
constexpr uint8_t SPECTRUM_RIDGE_STEM_PERCENT = 46U;
constexpr int16_t SPECTRUM_RIDGE_STEM_W = 1;

// P1.5.3.2R.6 SegmentedColumns：18列分段小矩形，继续使用同一个单对象自绘区域。
// 主柱与倒影每列都至少保留1格；Peak Hold 改成与主柱同宽、仅3px高的窄落点。
// 18列从现有24点视觉频谱等距取样，既覆盖完整频段，也控制每帧小矩形数量。
constexpr uint8_t SPECTRUM_SEGMENTED_COLUMN_COUNT = 18U;
constexpr int16_t SPECTRUM_SEGMENTED_BAR_W = 11;
constexpr int16_t SPECTRUM_SEGMENTED_BAR_GAP = 8;
constexpr int16_t SPECTRUM_SEGMENTED_BASELINE_Y = SPECTRUM_AREA_H / 2;
constexpr int16_t SPECTRUM_SEGMENTED_BASELINE_GAP = 3;
constexpr int16_t SPECTRUM_SEGMENTED_SEGMENT_H = 5;
constexpr int16_t SPECTRUM_SEGMENTED_SEGMENT_GAP = 3;
constexpr int16_t SPECTRUM_SEGMENTED_SEGMENT_STEP =
    SPECTRUM_SEGMENTED_SEGMENT_H + SPECTRUM_SEGMENTED_SEGMENT_GAP;
constexpr int16_t SPECTRUM_SEGMENTED_MAX_H = 96;
constexpr uint8_t SPECTRUM_SEGMENTED_REFLECTION_PERCENT = 46U;
constexpr int16_t SPECTRUM_SEGMENTED_PEAK_H = 3;
constexpr int16_t SPECTRUM_SEGMENTED_PEAK_GAP = 6;

// P1.5.2R.4：参考目标机视觉，颜色只随横向位置变化。
// 使用固定24色色阶避免每帧做颜色插值：洋红 -> 紫 -> 蓝 -> 青。
constexpr uint32_t SPECTRUM_GRADIENT[SPECTRUM_VISUAL_BAR_COUNT] = {
    0xFF30B8, 0xF833C0, 0xF037C8, 0xE93AD0, 0xE13ED8, 0xDA41E1,
    0xD245E9, 0xCB48F1, 0xC44BF9, 0xBA4FFF, 0xAB53FF, 0x9B57FF,
    0x8C5AFF, 0x7D5EFF, 0x6D62FF, 0x5E66FF, 0x526EFE, 0x4C7EFA,
    0x458EF6, 0x3F9EF2, 0x39AEEE, 0x33BEEA, 0x2CCEE6, 0x26DEE2,
};

// P1.5.2R.4.1：倒影使用预先压暗约42%的固定色阶。
// 不用每帧 alpha blend，保持单对象自绘路径足够轻。
constexpr uint32_t SPECTRUM_REFLECTION_GRADIENT[SPECTRUM_VISUAL_BAR_COUNT] = {
    0x6B144D, 0x681550, 0x641754, 0x611857, 0x5E1A5A, 0x5B1B5E,
    0x581C61, 0x551E65, 0x521F68, 0x4E216B, 0x47226B, 0x41246B,
    0x3A256B, 0x34276B, 0x2D296B, 0x272A6B, 0x222E6A, 0x1F3469,
    0x1C3B67, 0x1A4265, 0x174963, 0x154F62, 0x125660, 0x0F5D5E,
};

lv_obj_t *g_root = nullptr;
lv_obj_t *g_title = nullptr;
lv_obj_t *g_artist = nullptr;
lv_obj_t *g_time = nullptr;
lv_obj_t *g_spectrum_widget = nullptr;
lv_obj_t *g_lyric = nullptr;
lv_timer_t *g_timer = nullptr;
lv_timer_t *g_delayed_start_timer = nullptr;
bool g_visible = false;
// 防烧屏：暂停/停止时频谱整体收起（高度0不画），避免固定亮色条长期定格在屏上。
bool g_idle_collapsed = true;
uint32_t g_frame = 0U;
uint32_t g_last_pcm_revision = 0U;
uint32_t g_last_track = UINT32_MAX;
uint32_t g_lyrics_requested_track = UINT32_MAX;
uint32_t g_last_lyrics_document_revision = 0U;
uint32_t g_last_lyrics_line = UINT32_MAX;
uint16_t g_bar_height[SPECTRUM_BAR_COUNT] = {};
uint16_t g_bar_target[SPECTRUM_BAR_COUNT] = {};
uint16_t g_peak_height[SPECTRUM_VISUAL_BAR_COUNT] = {};
// R.31：绘制层几何缓存。原实现会在每个 draw primitive 内重复做 16→24 / 16→40
// 插值、比例换算和 Neon 颜色插值；这些值每帧只需要计算一次。
uint16_t g_visual_height_cache[SPECTRUM_VISUAL_BAR_COUNT] = {};
int16_t g_segmented_height_cache[SPECTRUM_SEGMENTED_COLUMN_COUNT] = {};
int16_t g_segmented_peak_cache[SPECTRUM_SEGMENTED_COLUMN_COUNT] = {};
int16_t g_ridge_height_cache[SPECTRUM_RIDGE_POINT_COUNT] = {};
uint32_t g_ridge_color_cache[SPECTRUM_RIDGE_POINT_COUNT] = {};
uint32_t g_ridge_trail_color_cache[SPECTRUM_RIDGE_POINT_COUNT] = {};
uint32_t g_ridge_stem_color_cache[SPECTRUM_RIDGE_POINT_COUNT] = {};
bool g_ridge_color_cache_ready = false;
uint64_t g_last_displayed_second = UINT64_MAX;
SpectrumStyle g_style = SpectrumStyle::SegmentedColumns;

static void spectrum_format_time(uint64_t ms, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0U) {
        return;
    }
    const uint64_t seconds = ms / 1000ULL;
    const uint64_t minutes = seconds / 60ULL;
    snprintf(out, out_size, "%llu:%02llu",
        static_cast<unsigned long long>(minutes),
        static_cast<unsigned long long>(seconds % 60ULL));
}

static lv_obj_t *spectrum_create_label(
    lv_obj_t *parent,
    const char *text,
    lv_color_t color,
    int32_t width,
    int32_t height)
{
    lv_obj_t *label = lv_label_create(parent);
    ui_common_lock_object(label);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, font_manager_get_ui_font(), 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(label, 0, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text != nullptr ? text : "");
    return label;
}

static void spectrum_refresh_header(uint32_t track_index)
{
    if (g_title == nullptr || g_artist == nullptr || track_index == g_last_track) {
        return;
    }

    const char *title = "未选择歌曲";
    const char *artist = "";
    MediaTrackViewV2 view = {};
    if (track_index != UINT32_MAX && media_catalog_v2_get_track_view(track_index, &view)) {
        if (view.title != nullptr && view.title[0] != '\0') {
            title = view.title;
        }
        if (view.artist != nullptr && view.artist[0] != '\0') {
            artist = view.artist;
        }
    }

    lv_label_set_text(g_title, title);
    lv_label_set_text(g_artist, artist);
    g_last_track = track_index;
}

static uint32_t spectrum_current_track(const AudioStateSnapshot &audio)
{
    if (audio.track_index != UINT32_MAX) {
        return audio.track_index;
    }
    size_t selected = 0U;
    return player_playlist_get_track_index(&selected)
        ? static_cast<uint32_t>(selected)
        : UINT32_MAX;
}

static const char *spectrum_style_name(SpectrumStyle style)
{
    switch (style) {
        case SpectrumStyle::SegmentedColumns:
            return "SegmentedColumns";
        case SpectrumStyle::HorizontalMirror:
            return "HorizontalMirror";
        case SpectrumStyle::NeonRidge:
        default:
            return "NeonRidge";
    }
}

static void spectrum_apply_style_layout()
{
    if (g_root == nullptr || g_title == nullptr || g_artist == nullptr ||
        g_time == nullptr || g_spectrum_widget == nullptr || g_lyric == nullptr) {
        return;
    }

    // 三种样式都保留歌曲名 / 歌手 / 当前歌词 / 时间。歌词规则保持不变：短句单行，超长才均衡拆成两行。
    lv_obj_set_size(g_title, 392, 34);
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_text_color(g_title, lv_color_hex(0xFFFFFF), 0);

    lv_obj_set_size(g_artist, 392, 30);
    lv_obj_align(g_artist, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_text_color(g_artist, lv_color_hex(0x8995A6), 0);

    lv_obj_set_pos(g_lyric, SPECTRUM_LYRIC_X, SPECTRUM_LYRIC_Y);
    lv_obj_set_size(g_lyric, SPECTRUM_LYRIC_W, SPECTRUM_LYRIC_H);
    lv_obj_remove_flag(g_lyric, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_size(g_time, 180, 30);
    lv_obj_align(g_time, LV_ALIGN_BOTTOM_MID, 0, -38);
    lv_obj_set_style_text_color(g_time, lv_color_hex(0x7E8998), 0);

    if (g_style == SpectrumStyle::NeonRidge) {
        lv_obj_set_pos(g_spectrum_widget, SPECTRUM_RIDGE_X, SPECTRUM_RIDGE_Y);
        lv_obj_set_size(g_spectrum_widget, SPECTRUM_RIDGE_W, SPECTRUM_RIDGE_H);
    } else {
        lv_obj_set_pos(g_spectrum_widget, SPECTRUM_AREA_LEFT, SPECTRUM_AREA_TOP);
        lv_obj_set_size(g_spectrum_widget, SPECTRUM_AREA_W, SPECTRUM_AREA_H);
    }

    // 文字始终位于频谱自绘对象前景。
    lv_obj_move_foreground(g_title);
    lv_obj_move_foreground(g_artist);
    lv_obj_move_foreground(g_lyric);
    lv_obj_move_foreground(g_time);
    lv_obj_invalidate(g_spectrum_widget);
}


static size_t spectrum_decode_utf8(const uint8_t *p, uint32_t *codepoint)
{
    if (p == nullptr || codepoint == nullptr || p[0] == 0U) {
        return 0U;
    }
    if (p[0] < 0x80U) {
        *codepoint = p[0];
        return 1U;
    }
    if ((p[0] & 0xE0U) == 0xC0U && p[1] != 0U && (p[1] & 0xC0U) == 0x80U) {
        const uint32_t cp = ((p[0] & 0x1FU) << 6U) | (p[1] & 0x3FU);
        if (cp >= 0x80U) {
            *codepoint = cp;
            return 2U;
        }
    }
    if ((p[0] & 0xF0U) == 0xE0U && p[1] != 0U && p[2] != 0U &&
        (p[1] & 0xC0U) == 0x80U && (p[2] & 0xC0U) == 0x80U) {
        const uint32_t cp = ((p[0] & 0x0FU) << 12U) |
            ((p[1] & 0x3FU) << 6U) | (p[2] & 0x3FU);
        if (cp >= 0x800U && !(cp >= 0xD800U && cp <= 0xDFFFU)) {
            *codepoint = cp;
            return 3U;
        }
    }
    if ((p[0] & 0xF8U) == 0xF0U && p[1] != 0U && p[2] != 0U && p[3] != 0U &&
        (p[1] & 0xC0U) == 0x80U && (p[2] & 0xC0U) == 0x80U && (p[3] & 0xC0U) == 0x80U) {
        const uint32_t cp = ((p[0] & 0x07U) << 18U) |
            ((p[1] & 0x3FU) << 12U) | ((p[2] & 0x3FU) << 6U) | (p[3] & 0x3FU);
        if (cp >= 0x10000U && cp <= 0x10FFFFU) {
            *codepoint = cp;
            return 4U;
        }
    }
    return 0U;
}

static int32_t spectrum_measure_text_range(const char *begin, const char *end)
{
    if (begin == nullptr || end == nullptr || begin >= end) {
        return 0;
    }
    const lv_font_t *font = font_manager_get_ui_font();
    if (font == nullptr || font->get_glyph_dsc == nullptr) {
        return static_cast<int32_t>(end - begin) * 12;
    }

    int32_t width = 0;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(begin);
    const uint8_t *limit = reinterpret_cast<const uint8_t *>(end);
    while (p < limit && *p != 0U) {
        uint32_t cp = 0U;
        const size_t used = spectrum_decode_utf8(p, &cp);
        if (used == 0U || p + used > limit) {
            ++p;
            continue;
        }
        uint32_t next_cp = 0U;
        if (p + used < limit) {
            spectrum_decode_utf8(p + used, &next_cp);
        }
        width += static_cast<int32_t>(lv_font_get_glyph_width(font, cp, next_cp));
        p += used;
    }
    return width;
}

static int32_t spectrum_measure_text_width(const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    return spectrum_measure_text_range(text, text + strlen(text));
}

static bool spectrum_is_ascii_space(uint32_t cp)
{
    return cp == ' ' || cp == '\t';
}

static bool spectrum_is_preferred_break_after(uint32_t cp)
{
    switch (cp) {
        case ',': case '.': case '!': case '?': case ';': case ':':
        case 0x3001U: // 、
        case 0x3002U: // 。
        case 0xFF0CU: // ，
        case 0xFF0EU: // ．
        case 0xFF01U: // ！
        case 0xFF1FU: // ？
        case 0xFF1BU: // ；
        case 0xFF1AU: // ：
            return true;
        default:
            return false;
    }
}

static bool spectrum_is_bad_second_line_start(uint32_t cp)
{
    switch (cp) {
        case ',': case '.': case '!': case '?': case ';': case ':':
        case ')': case ']': case '}':
        case 0x3001U: case 0x3002U:
        case 0xFF0CU: case 0xFF0EU: case 0xFF01U: case 0xFF1FU:
        case 0xFF1BU: case 0xFF1AU:
        case 0xFF09U: // ）
        case 0x3011U: // 】
        case 0x300BU: // 》
        case 0x300DU: // 」
        case 0x300FU: // 』
            return true;
        default:
            return false;
    }
}

static size_t spectrum_copy_trimmed_range(
    char *out,
    size_t out_size,
    const char *begin,
    const char *end)
{
    if (out == nullptr || out_size == 0U || begin == nullptr || end == nullptr || begin >= end) {
        if (out != nullptr && out_size > 0U) {
            out[0] = '\0';
        }
        return 0U;
    }
    while (begin < end && (*begin == ' ' || *begin == '\t')) {
        ++begin;
    }
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }
    const size_t bytes = static_cast<size_t>(end - begin);
    const size_t copy_bytes = bytes < out_size - 1U ? bytes : out_size - 1U;
    memcpy(out, begin, copy_bytes);
    out[copy_bytes] = '\0';
    return copy_bytes;
}

static void spectrum_truncate_to_width(char *text, size_t text_size, int32_t max_width)
{
    if (text == nullptr || text_size == 0U || text[0] == '\0' ||
        spectrum_measure_text_width(text) <= max_width) {
        return;
    }

    constexpr char suffix[] = "...";
    const int32_t suffix_width = spectrum_measure_text_width(suffix);
    const int32_t content_limit = max_width > suffix_width ? max_width - suffix_width : max_width;
    const char *begin = text;
    const char *p = text;
    const char *best_end = text;
    while (*p != '\0') {
        uint32_t cp = 0U;
        const size_t used = spectrum_decode_utf8(reinterpret_cast<const uint8_t *>(p), &cp);
        if (used == 0U) {
            ++p;
            continue;
        }
        const char *candidate_end = p + used;
        if (spectrum_measure_text_range(begin, candidate_end) > content_limit) {
            break;
        }
        best_end = candidate_end;
        p = candidate_end;
    }

    char tmp[SPECTRUM_LYRIC_FORMATTED_BYTES] = {};
    const size_t kept = spectrum_copy_trimmed_range(tmp, sizeof(tmp), begin, best_end);
    if (kept + sizeof(suffix) <= sizeof(tmp)) {
        memcpy(tmp + kept, suffix, sizeof(suffix));
    }
    snprintf(text, text_size, "%s", tmp);
}

static void spectrum_format_balanced_lyric(
    const char *text,
    char *out,
    size_t out_size)
{
    if (out == nullptr || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    const size_t total_bytes = strlen(text);
    const char *text_end = text + total_bytes;
    if (spectrum_measure_text_width(text) <= SPECTRUM_LYRIC_TEXT_MAX_W) {
        snprintf(out, out_size, "%s", text);
        return;
    }

    struct BreakChoice {
        const char *first_end = nullptr;
        const char *second_begin = nullptr;
        int32_t score = INT32_MAX;
        bool both_fit = false;
    } best;

    const char *p = text;
    while (p < text_end && *p != '\0') {
        uint32_t cp = 0U;
        const size_t used = spectrum_decode_utf8(reinterpret_cast<const uint8_t *>(p), &cp);
        if (used == 0U) {
            ++p;
            continue;
        }
        const char *after = p + used;
        if (after >= text_end) {
            break;
        }

        const char *first_end = after;
        const char *second_begin = after;
        bool preferred = spectrum_is_preferred_break_after(cp);
        if (spectrum_is_ascii_space(cp)) {
            first_end = p;
            second_begin = after;
            preferred = true;
            while (second_begin < text_end && (*second_begin == ' ' || *second_begin == '\t')) {
                ++second_begin;
            }
        }
        if (first_end <= text || second_begin >= text_end) {
            p = after;
            continue;
        }

        uint32_t second_cp = 0U;
        spectrum_decode_utf8(reinterpret_cast<const uint8_t *>(second_begin), &second_cp);
        const int32_t first_w = spectrum_measure_text_range(text, first_end);
        const int32_t second_w = spectrum_measure_text_range(second_begin, text_end);
        const bool first_fit = first_w <= SPECTRUM_LYRIC_TEXT_MAX_W;
        const bool both_fit = first_fit && second_w <= SPECTRUM_LYRIC_TEXT_MAX_W;
        if (!both_fit && !first_fit) {
            // 极端超长歌词也不能截掉中间内容：fallback 只允许第一行完整放下，
            // 第二行若仍超宽则在末尾省略。
            p = after;
            continue;
        }

        const int32_t imbalance = first_w > second_w ? first_w - second_w : second_w - first_w;

        // 完整两行能放下时，以两边实际像素宽度尽量接近为第一目标；
        // 若总长度确实超过两行容量，则第一行尽量利用宽度、第二行只截末尾，
        // 从而保证歌词内容始终按原顺序连续，不会在断点附近丢掉中间字符。
        int32_t score = both_fit
            ? imbalance
            : (SPECTRUM_LYRIC_TEXT_MAX_W - first_w);
        if (preferred) {
            score -= 18;
        }
        if (spectrum_is_bad_second_line_start(second_cp)) {
            score += 80;
        }

        if ((both_fit && !best.both_fit) ||
            (both_fit == best.both_fit && score < best.score)) {
            best.first_end = first_end;
            best.second_begin = second_begin;
            best.score = score;
            best.both_fit = both_fit;
        }
        p = after;
    }

    if (best.first_end == nullptr || best.second_begin == nullptr) {
        snprintf(out, out_size, "%s", text);
        return;
    }

    char first[SPECTRUM_LYRIC_FORMATTED_BYTES] = {};
    char second[SPECTRUM_LYRIC_FORMATTED_BYTES] = {};
    spectrum_copy_trimmed_range(first, sizeof(first), text, best.first_end);
    spectrum_copy_trimmed_range(second, sizeof(second), best.second_begin, text_end);

    // 极端超长歌词超过两行容量时，fallback 已保证第一行完整可见；
    // 第二行只在末尾加省略号，不额外产生第三行，也不丢中间内容。
    spectrum_truncate_to_width(second, sizeof(second), SPECTRUM_LYRIC_TEXT_MAX_W);
    snprintf(out, out_size, "%s\n%s", first, second);
}

static void spectrum_clear_current_lyric(bool reset_request)
{
    g_last_lyrics_document_revision = 0U;
    g_last_lyrics_line = UINT32_MAX;
    if (reset_request) {
        g_lyrics_requested_track = UINT32_MAX;
    }
    if (g_lyric != nullptr) {
        lv_label_set_text(g_lyric, "");
    }
}

static void spectrum_request_lyrics_if_needed(
    uint32_t track_index,
    const LyricsWindowSnapshot &window)
{
    if (!lyrics_service_is_ready() || track_index == UINT32_MAX) {
        return;
    }

    // LyricsService 是共享数据服务。若歌词页刚加载过同一首，这里直接复用，
    // 不重复读取 LRC；只有服务当前目标不是本曲时才提交请求。
    if (window.track_index == track_index && window.state != LyricsLoadState::Idle) {
        g_lyrics_requested_track = track_index;
        return;
    }
    if (g_lyrics_requested_track == track_index) {
        return;
    }
    if (lyrics_service_request_track(track_index)) {
        g_lyrics_requested_track = track_index;
    }
}

static void spectrum_update_current_lyric(
    const AudioStateSnapshot &audio,
    uint32_t track_index)
{
    if (g_lyric == nullptr || !lyrics_service_is_ready() || track_index == UINT32_MAX) {
        spectrum_clear_current_lyric(false);
        return;
    }

    const uint64_t position_ms = audio.track_index == track_index ? audio.position_ms : 0U;
    LyricsWindowSnapshot window = {};
    if (!lyrics_service_get_window(track_index, position_ms, &window)) {
        return;
    }

    spectrum_request_lyrics_if_needed(track_index, window);

    // 请求刚切到新曲、正在加载、无歌词或编码不支持时保持纯黑空白，
    // 不在频谱页显示“暂无歌词/加载中”等状态文案。
    if (window.track_index != track_index || window.state != LyricsLoadState::Ready) {
        if (g_last_lyrics_line != UINT32_MAX || g_last_lyrics_document_revision != window.revision) {
            spectrum_clear_current_lyric(false);
            g_last_lyrics_document_revision = window.revision;
        }
        return;
    }

    const LyricsWindowLine &current = window.lines[2];
    const bool have_current = current.valid && current.current && current.text[0] != '\0';
    if (!have_current) {
        if (g_last_lyrics_line != UINT32_MAX || g_last_lyrics_document_revision != window.revision) {
            spectrum_clear_current_lyric(false);
            g_last_lyrics_document_revision = window.revision;
        }
        return;
    }

    // 只在文档或当前歌词行真正变化时触碰 LVGL 文本。
    if (window.revision == g_last_lyrics_document_revision &&
        window.current_line_index == g_last_lyrics_line) {
        return;
    }

    g_last_lyrics_document_revision = window.revision;
    g_last_lyrics_line = window.current_line_index;

    char formatted[SPECTRUM_LYRIC_FORMATTED_BYTES] = {};
    spectrum_format_balanced_lyric(current.text, formatted, sizeof(formatted));
    lv_label_set_text(g_lyric, formatted);
}

static void spectrum_set_idle_targets()
{
    for (uint8_t i = 0U; i < SPECTRUM_BAR_COUNT; ++i) {
        g_bar_target[i] = SPECTRUM_MIN_H;
    }
}

static bool spectrum_snapshot_matches(
    const AudioSpectrumSnapshot &spectrum,
    const AudioStateSnapshot &audio)
{
    return
        spectrum.valid &&
        spectrum.playback_revision == audio.playback_revision &&
        spectrum.track_index == audio.track_index &&
        spectrum.sample_rate_hz == audio.sample_rate_hz;
}

static void spectrum_apply_pcm_snapshot(const AudioSpectrumSnapshot &spectrum)
{
    for (uint8_t i = 0U; i < SPECTRUM_BAR_COUNT; ++i) {
        // R.35.2：低频 1.00x → 高频 1.50x，且增益主要集中在高频区。
        // 先对 0~255 的显示 level 做饱和增益，再映射为像素高度；这样三种频谱样式
        // 共享完全一致的频响视觉补偿，同时保持 AudioSpectrumSnapshot 原始数据不变。
        uint32_t level =
            (static_cast<uint32_t>(spectrum.levels[i]) * SPECTRUM_BAND_GAIN_Q8[i] + 128U) >> 8U;
        if (level > 255U) {
            level = 255U;
        }
        const uint32_t height = SPECTRUM_MIN_H +
            (level * static_cast<uint32_t>(SPECTRUM_MAX_H - SPECTRUM_MIN_H)) / 255U;
        g_bar_target[i] = static_cast<uint16_t>(height);
    }
}

static uint16_t spectrum_visual_height_uncached(uint8_t visual_index)
{
    if (visual_index >= SPECTRUM_VISUAL_BAR_COUNT) {
        return SPECTRUM_MIN_H;
    }
    const uint32_t pos =
        (static_cast<uint32_t>(visual_index) *
            static_cast<uint32_t>(SPECTRUM_BAR_COUNT - 1U) << 8U) /
        static_cast<uint32_t>(SPECTRUM_VISUAL_BAR_COUNT - 1U);
    const uint8_t left = static_cast<uint8_t>(pos >> 8U);
    const uint8_t right = left + 1U < SPECTRUM_BAR_COUNT ? left + 1U : left;
    const uint32_t frac = pos & 0xFFU;
    const uint32_t a = g_bar_height[left];
    const uint32_t b = g_bar_height[right];
    return static_cast<uint16_t>((a * (256U - frac) + b * frac) >> 8U);
}

static uint16_t spectrum_visual_height(uint8_t visual_index)
{
    return visual_index < SPECTRUM_VISUAL_BAR_COUNT
        ? g_visual_height_cache[visual_index]
        : SPECTRUM_MIN_H;
}

static void spectrum_rebuild_visual_cache()
{
    for (uint8_t i = 0U; i < SPECTRUM_VISUAL_BAR_COUNT; ++i) {
        g_visual_height_cache[i] = spectrum_visual_height_uncached(i);
    }
}

static void spectrum_draw_horizontal(lv_layer_t *layer, lv_obj_t *obj)
{
    lv_area_t coords = {};
    lv_obj_get_coords(obj, &coords);

    constexpr int32_t bars_total_w =
        SPECTRUM_VISUAL_BAR_COUNT * SPECTRUM_BAR_W +
        (SPECTRUM_VISUAL_BAR_COUNT - 1) * SPECTRUM_BAR_GAP;
    const int32_t bars_left = coords.x1 + (SPECTRUM_AREA_W - bars_total_w) / 2;
    const int32_t baseline_y = coords.y1 + (SPECTRUM_AREA_H / 2);

    // 中央基线只画一次，保持克制，不使用大面积Alpha/Glow。
    lv_draw_rect_dsc_t baseline_dsc = {};
    lv_draw_rect_dsc_init(&baseline_dsc);
    baseline_dsc.bg_color = lv_color_hex(0x252A38);
    baseline_dsc.bg_opa = LV_OPA_COVER;
    baseline_dsc.radius = 1;
    baseline_dsc.border_width = 0;

    lv_area_t baseline_area = {};
    baseline_area.x1 = bars_left - 4;
    baseline_area.x2 = bars_left + bars_total_w + 3;
    baseline_area.y1 = baseline_y;
    baseline_area.y2 = baseline_y + 1;
    lv_draw_rect(layer, &baseline_dsc, &baseline_area);

    // 防烧屏：暂停/停止时音柱整体收起（不画），只保留暗基线。
    if (g_idle_collapsed) {
        return;
    }

    lv_draw_rect_dsc_t bar_dsc = {};
    lv_draw_rect_dsc_init(&bar_dsc);
    bar_dsc.bg_opa = LV_OPA_COVER;
    bar_dsc.radius = 2;
    bar_dsc.border_width = 0;

    for (uint8_t i = 0U; i < SPECTRUM_VISUAL_BAR_COUNT; ++i) {
        const int32_t height = spectrum_visual_height(i);
        const int32_t x1 = bars_left + static_cast<int32_t>(i) * (SPECTRUM_BAR_W + SPECTRUM_BAR_GAP);
        const int32_t x2 = x1 + SPECTRUM_BAR_W - 1;

        // 上半主音柱：横向固定渐变色，作为真实频谱主体。
        bar_dsc.bg_color = lv_color_hex(SPECTRUM_GRADIENT[i]);

        lv_area_t upper = {};
        upper.x1 = x1;
        upper.x2 = x2;
        upper.y2 = baseline_y - SPECTRUM_BASELINE_GAP - 1;
        upper.y1 = upper.y2 - height + 1;
        lv_draw_rect(layer, &bar_dsc, &upper);

        // 独立 Peak Dot：峰值被新音柱顶高后，以固定速度向下“降落”。
        const int32_t peak_h = static_cast<int32_t>(g_peak_height[i]);
        if (peak_h > SPECTRUM_MIN_H) {
            lv_area_t peak = {};
            const int32_t peak_center_x = x1 + (SPECTRUM_BAR_W / 2);
            peak.x1 = peak_center_x - (SPECTRUM_PEAK_DOT_W / 2);
            peak.x2 = peak.x1 + SPECTRUM_PEAK_DOT_W - 1;
            peak.y2 = upper.y2 - peak_h - SPECTRUM_PEAK_DOT_GAP + 1;
            peak.y1 = peak.y2 - SPECTRUM_PEAK_DOT_H + 1;
            lv_draw_rect(layer, &bar_dsc, &peak);
        }

        // 下半只做倒影：高度约为上半55%，并使用压暗后的同色系。
        const int32_t reflection_h =
            (height * static_cast<int32_t>(SPECTRUM_REFLECTION_PERCENT) + 50) / 100;
        bar_dsc.bg_color = lv_color_hex(SPECTRUM_REFLECTION_GRADIENT[i]);

        lv_area_t lower = {};
        lower.x1 = x1;
        lower.x2 = x2;
        lower.y1 = baseline_y + SPECTRUM_BASELINE_GAP + 1;
        lower.y2 = lower.y1 + reflection_h - 1;
        lv_draw_rect(layer, &bar_dsc, &lower);
    }
}

static uint16_t spectrum_ridge_source_height_uncached(uint8_t point_index)
{
    if (point_index >= SPECTRUM_RIDGE_POINT_COUNT) {
        return SPECTRUM_MIN_H;
    }

    // 16个真实FFT band -> 40个连续山脊点。
    // 使用线性插值保留瞬态，不再额外做重型平滑；绘制连线本身形成连续轮廓。
    const uint32_t pos =
        (static_cast<uint32_t>(point_index) *
            static_cast<uint32_t>(SPECTRUM_BAR_COUNT - 1U) << 8U) /
        static_cast<uint32_t>(SPECTRUM_RIDGE_POINT_COUNT - 1U);
    const uint8_t left = static_cast<uint8_t>(pos >> 8U);
    const uint8_t right = left + 1U < SPECTRUM_BAR_COUNT ? left + 1U : left;
    const uint32_t frac = pos & 0xFFU;
    const uint32_t a = g_bar_height[left];
    const uint32_t b = g_bar_height[right];
    return static_cast<uint16_t>((a * (256U - frac) + b * frac) >> 8U);
}

static uint32_t spectrum_ridge_gradient_rgb(uint8_t point_index)
{
    // 横向四锚点：洋红 -> 紫 -> 蓝 -> 青。
    constexpr uint32_t anchors[4] = {0xFF30B8, 0xB94FFF, 0x4C7EFA, 0x26DEE2};
    const uint32_t scaled =
        (static_cast<uint32_t>(point_index) * 3U << 8U) /
        static_cast<uint32_t>(SPECTRUM_RIDGE_POINT_COUNT - 1U);
    uint8_t segment = static_cast<uint8_t>(scaled >> 8U);
    if (segment > 2U) {
        segment = 2U;
    }
    const uint32_t frac = scaled & 0xFFU;
    const uint32_t c0 = anchors[segment];
    const uint32_t c1 = anchors[segment + 1U];
    const uint32_t r = (((c0 >> 16U) & 0xFFU) * (256U - frac) + ((c1 >> 16U) & 0xFFU) * frac) >> 8U;
    const uint32_t g = (((c0 >> 8U) & 0xFFU) * (256U - frac) + ((c1 >> 8U) & 0xFFU) * frac) >> 8U;
    const uint32_t b = ((c0 & 0xFFU) * (256U - frac) + (c1 & 0xFFU) * frac) >> 8U;
    return (r << 16U) | (g << 8U) | b;
}

static uint32_t spectrum_scale_rgb(uint32_t rgb, uint8_t percent)
{
    const uint32_t r = (((rgb >> 16U) & 0xFFU) * percent + 50U) / 100U;
    const uint32_t g = (((rgb >> 8U) & 0xFFU) * percent + 50U) / 100U;
    const uint32_t b = ((rgb & 0xFFU) * percent + 50U) / 100U;
    return (r << 16U) | (g << 8U) | b;
}

static void spectrum_init_static_draw_cache()
{
    if (g_ridge_color_cache_ready) {
        return;
    }
    for (uint8_t i = 0U; i < SPECTRUM_RIDGE_POINT_COUNT; ++i) {
        const uint32_t rgb = spectrum_ridge_gradient_rgb(i);
        g_ridge_color_cache[i] = rgb;
        g_ridge_trail_color_cache[i] = spectrum_scale_rgb(rgb, SPECTRUM_RIDGE_TRAIL_PERCENT);
        g_ridge_stem_color_cache[i] = spectrum_scale_rgb(rgb, SPECTRUM_RIDGE_STEM_PERCENT);
    }
    g_ridge_color_cache_ready = true;
}

static int16_t spectrum_ridge_scale_height(uint16_t source)
{
    constexpr int32_t source_span = SPECTRUM_MAX_H - SPECTRUM_MIN_H;
    constexpr int32_t ridge_span = SPECTRUM_RIDGE_MAX_H - SPECTRUM_RIDGE_MIN_H;
    if (source_span <= 0) {
        return SPECTRUM_RIDGE_MIN_H;
    }
    int32_t h = SPECTRUM_RIDGE_MIN_H +
        ((static_cast<int32_t>(source) - SPECTRUM_MIN_H) * ridge_span + source_span / 2) / source_span;
    if (h < SPECTRUM_RIDGE_MIN_H) h = SPECTRUM_RIDGE_MIN_H;
    if (h > SPECTRUM_RIDGE_MAX_H) h = SPECTRUM_RIDGE_MAX_H;
    return static_cast<int16_t>(h);
}

static int16_t spectrum_ridge_height_px(uint8_t point_index)
{
    return point_index < SPECTRUM_RIDGE_POINT_COUNT
        ? g_ridge_height_cache[point_index]
        : SPECTRUM_RIDGE_MIN_H;
}

static void spectrum_draw_neon_ridge(lv_layer_t *layer, lv_obj_t *obj)
{
    if (layer == nullptr || obj == nullptr) {
        return;
    }

    lv_area_t coords = {};
    lv_obj_get_coords(obj, &coords);
    const int32_t x_step_q8 =
        ((SPECTRUM_RIDGE_W - 1) << 8) / static_cast<int32_t>(SPECTRUM_RIDGE_POINT_COUNT - 1U);
    const int32_t baseline_y = coords.y1 + SPECTRUM_RIDGE_BASELINE_Y;

    // 极淡基线提供“山脚”参照，但不抢霓虹主轮廓。
    lv_draw_line_dsc_t baseline = {};
    lv_draw_line_dsc_init(&baseline);
    baseline.color = lv_color_hex(0x181C27);
    baseline.width = 1;
    baseline.opa = LV_OPA_COVER;
    baseline.p1.x = coords.x1;
    baseline.p1.y = baseline_y;
    baseline.p2.x = coords.x2;
    baseline.p2.y = baseline_y;
    lv_draw_line(layer, &baseline);

    // 防烧屏：暂停/停止时山脊/竖线整体收起（不画），只保留暗基线。
    if (g_idle_collapsed) {
        return;
    }

    // P1.5.3.2：先画细竖向 stems（每个山脊采样点一根），从基线连到实时山脊高度。
    // 它们与主山脊使用同一横向渐变，但固定压暗，既补足“频谱”结构感，
    // 又不重新创建独立LVGL bar对象。
    lv_draw_line_dsc_t stem = {};
    lv_draw_line_dsc_init(&stem);
    stem.width = SPECTRUM_RIDGE_STEM_W;
    stem.opa = LV_OPA_COVER;
    stem.round_start = 0U;
    stem.round_end = 0U;
    for (uint8_t i = 0U; i < SPECTRUM_RIDGE_POINT_COUNT; i = static_cast<uint8_t>(i + SPECTRUM_RIDGE_STEM_STEP)) {
        const int32_t x = coords.x1 + ((static_cast<int32_t>(i) * x_step_q8) >> 8);
        const int32_t ridge_y = baseline_y - spectrum_ridge_height_px(i);
        stem.color = lv_color_hex(g_ridge_stem_color_cache[i]);
        stem.p1.x = x; stem.p1.y = baseline_y - 1;
        stem.p2.x = x; stem.p2.y = ridge_y + 2;
        lv_draw_line(layer, &stem);
    }

    // 第一遍：在主线下方7px画低亮尾迹，制造“流光”纵深，不用Alpha/Blur。
    lv_draw_line_dsc_t line = {};
    lv_draw_line_dsc_init(&line);
    line.width = SPECTRUM_RIDGE_TRAIL_W;
    line.opa = LV_OPA_COVER;
    line.round_start = 1U;
    line.round_end = 1U;
    for (uint8_t i = 0U; i + 1U < SPECTRUM_RIDGE_POINT_COUNT; ++i) {
        const int32_t x0 = coords.x1 + ((static_cast<int32_t>(i) * x_step_q8) >> 8);
        const int32_t x1 = coords.x1 + ((static_cast<int32_t>(i + 1U) * x_step_q8) >> 8);
        const int32_t y0 = baseline_y - spectrum_ridge_height_px(i) + SPECTRUM_RIDGE_TRAIL_OFFSET_Y;
        const int32_t y1 = baseline_y - spectrum_ridge_height_px(i + 1U) + SPECTRUM_RIDGE_TRAIL_OFFSET_Y;
        line.color = lv_color_hex(g_ridge_trail_color_cache[i]);
        line.p1.x = x0; line.p1.y = y0;
        line.p2.x = x1; line.p2.y = y1;
        lv_draw_line(layer, &line);
    }

    // 第二遍：亮色主山脊。每个segment只做一次线段绘制，仍是单LVGL对象。
    line.width = SPECTRUM_RIDGE_MAIN_W;
    for (uint8_t i = 0U; i + 1U < SPECTRUM_RIDGE_POINT_COUNT; ++i) {
        const int32_t x0 = coords.x1 + ((static_cast<int32_t>(i) * x_step_q8) >> 8);
        const int32_t x1 = coords.x1 + ((static_cast<int32_t>(i + 1U) * x_step_q8) >> 8);
        const int32_t y0 = baseline_y - spectrum_ridge_height_px(i);
        const int32_t y1 = baseline_y - spectrum_ridge_height_px(i + 1U);
        line.color = lv_color_hex(g_ridge_color_cache[i]);
        line.p1.x = x0; line.p1.y = y0;
        line.p2.x = x1; line.p2.y = y1;
        lv_draw_line(layer, &line);
    }

    // 只标记少量明显局部峰值；不做每点Peak Hold，避免重新变成密集均衡器。
    lv_draw_rect_dsc_t dot = {};
    lv_draw_rect_dsc_init(&dot);
    dot.bg_opa = LV_OPA_COVER;
    dot.radius = SPECTRUM_RIDGE_PEAK_DOT;
    dot.border_width = 0;
    uint8_t emitted = 0U;
    for (uint8_t i = 2U; i + 2U < SPECTRUM_RIDGE_POINT_COUNT && emitted < 4U; ++i) {
        const int16_t h = spectrum_ridge_height_px(i);
        if (h < 52 || h <= spectrum_ridge_height_px(i - 1U) || h < spectrum_ridge_height_px(i + 1U)) {
            continue;
        }
        const int32_t x = coords.x1 + ((static_cast<int32_t>(i) * x_step_q8) >> 8);
        const int32_t y = baseline_y - h - 5;
        dot.bg_color = lv_color_hex(g_ridge_color_cache[i]);
        lv_area_t a = {x - 1, y - 1, x + 1, y + 1};
        lv_draw_rect(layer, &dot, &a);
        ++emitted;
        i = static_cast<uint8_t>(i + 3U);
    }
}

static uint8_t spectrum_segmented_visual_index(uint8_t column_index)
{
    if (column_index >= SPECTRUM_SEGMENTED_COLUMN_COUNT) {
        return 0U;
    }
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(column_index) *
            static_cast<uint32_t>(SPECTRUM_VISUAL_BAR_COUNT - 1U) +
            static_cast<uint32_t>((SPECTRUM_SEGMENTED_COLUMN_COUNT - 1U) / 2U)) /
        static_cast<uint32_t>(SPECTRUM_SEGMENTED_COLUMN_COUNT - 1U));
}

static int16_t spectrum_segmented_scale_height(uint16_t source_height)
{
    constexpr int32_t source_span = SPECTRUM_MAX_H - SPECTRUM_MIN_H;
    if (source_span <= 0 || source_height <= SPECTRUM_MIN_H) {
        return 0;
    }

    int32_t source = source_height;
    if (source > SPECTRUM_MAX_H) {
        source = SPECTRUM_MAX_H;
    }
    return static_cast<int16_t>(
        ((source - SPECTRUM_MIN_H) * SPECTRUM_SEGMENTED_MAX_H + source_span / 2) /
        source_span);
}

static int16_t spectrum_segmented_height_px(uint8_t column_index)
{
    return column_index < SPECTRUM_SEGMENTED_COLUMN_COUNT
        ? g_segmented_height_cache[column_index]
        : 0;
}

static int16_t spectrum_segmented_peak_height_px(uint8_t column_index)
{
    return column_index < SPECTRUM_SEGMENTED_COLUMN_COUNT
        ? g_segmented_peak_cache[column_index]
        : 0;
}

static void spectrum_rebuild_style_geometry_cache()
{
    if (g_style == SpectrumStyle::SegmentedColumns) {
        for (uint8_t i = 0U; i < SPECTRUM_SEGMENTED_COLUMN_COUNT; ++i) {
            const uint8_t visual_index = spectrum_segmented_visual_index(i);
            g_segmented_height_cache[i] =
                spectrum_segmented_scale_height(g_visual_height_cache[visual_index]);
            g_segmented_peak_cache[i] =
                spectrum_segmented_scale_height(g_peak_height[visual_index]);
        }
        return;
    }

    if (g_style == SpectrumStyle::NeonRidge) {
        for (uint8_t i = 0U; i < SPECTRUM_RIDGE_POINT_COUNT; ++i) {
            g_ridge_height_cache[i] =
                spectrum_ridge_scale_height(spectrum_ridge_source_height_uncached(i));
        }
    }
}

static void spectrum_draw_segmented_columns(lv_layer_t *layer, lv_obj_t *obj)
{
    if (layer == nullptr || obj == nullptr) {
        return;
    }

    lv_area_t coords = {};
    lv_obj_get_coords(obj, &coords);
    constexpr int32_t bars_total_w =
        SPECTRUM_SEGMENTED_COLUMN_COUNT * SPECTRUM_SEGMENTED_BAR_W +
        (SPECTRUM_SEGMENTED_COLUMN_COUNT - 1) * SPECTRUM_SEGMENTED_BAR_GAP;
    const int32_t bars_left = coords.x1 + (SPECTRUM_AREA_W - bars_total_w) / 2;
    const int32_t baseline_y = coords.y1 + SPECTRUM_SEGMENTED_BASELINE_Y;

    // 极暗1px基线只提供上下分界，主视觉仍由离散小矩形构成。
    lv_draw_rect_dsc_t baseline = {};
    lv_draw_rect_dsc_init(&baseline);
    baseline.bg_color = lv_color_hex(0x202431);
    baseline.bg_opa = LV_OPA_COVER;
    baseline.radius = 0;
    baseline.border_width = 0;
    lv_area_t baseline_area = {
        bars_left - 4, baseline_y, bars_left + bars_total_w + 3, baseline_y
    };
    lv_draw_rect(layer, &baseline, &baseline_area);

    // 防烧屏：暂停/停止时柱段整体收起（不画），只保留暗基线。
    if (g_idle_collapsed) {
        return;
    }

    lv_draw_rect_dsc_t segment = {};
    lv_draw_rect_dsc_init(&segment);
    segment.bg_opa = LV_OPA_COVER;
    segment.radius = 1;
    segment.border_width = 0;

    lv_draw_rect_dsc_t peak = {};
    lv_draw_rect_dsc_init(&peak);
    peak.bg_opa = LV_OPA_COVER;
    peak.radius = 0;
    peak.border_width = 0;

    for (uint8_t i = 0U; i < SPECTRUM_SEGMENTED_COLUMN_COUNT; ++i) {
        const uint8_t visual_index = spectrum_segmented_visual_index(i);
        const int32_t height = spectrum_segmented_height_px(i);
        const int32_t x1 = bars_left +
            static_cast<int32_t>(i) * (SPECTRUM_SEGMENTED_BAR_W + SPECTRUM_SEGMENTED_BAR_GAP);
        const int32_t x2 = x1 + SPECTRUM_SEGMENTED_BAR_W - 1;
        int32_t main_segments =
            (height + SPECTRUM_SEGMENTED_SEGMENT_STEP - 1) / SPECTRUM_SEGMENTED_SEGMENT_STEP;
        if (main_segments < 1) {
            main_segments = 1;
        }

        // 主频谱：一列由多个互不相连的小矩形向上堆叠。
        segment.bg_color = lv_color_hex(SPECTRUM_GRADIENT[visual_index]);
        for (int32_t s = 0; s < main_segments; ++s) {
            const int32_t bottom = baseline_y - SPECTRUM_SEGMENTED_BASELINE_GAP - 1 -
                s * SPECTRUM_SEGMENTED_SEGMENT_STEP;
            lv_area_t a = {
                x1,
                bottom - SPECTRUM_SEGMENTED_SEGMENT_H + 1,
                x2,
                bottom,
            };
            lv_draw_rect(layer, &segment, &a);
        }

        // 倒影：同列同色系但明显压暗，并且高度只保留主柱约46%。
        const int32_t reflection_h =
            (height * static_cast<int32_t>(SPECTRUM_SEGMENTED_REFLECTION_PERCENT) + 50) / 100;
        int32_t reflection_segments =
            (reflection_h + SPECTRUM_SEGMENTED_SEGMENT_STEP - 1) /
            SPECTRUM_SEGMENTED_SEGMENT_STEP;
        if (reflection_segments < 1) {
            reflection_segments = 1;
        }
        segment.bg_color = lv_color_hex(SPECTRUM_REFLECTION_GRADIENT[visual_index]);
        for (int32_t s = 0; s < reflection_segments; ++s) {
            const int32_t top = baseline_y + SPECTRUM_SEGMENTED_BASELINE_GAP + 1 +
                s * SPECTRUM_SEGMENTED_SEGMENT_STEP;
            lv_area_t a = {
                x1,
                top,
                x2,
                top + SPECTRUM_SEGMENTED_SEGMENT_H - 1,
            };
            lv_draw_rect(layer, &segment, &a);
        }

        // 窄落点：复用原有 Peak Hold，宽度与主柱一致，高度仅3px；无新峰值时按现有3px/帧独立下落。
        const int32_t peak_h = spectrum_segmented_peak_height_px(i);
        if (peak_h > 0) {
            const int32_t peak_y = baseline_y - SPECTRUM_SEGMENTED_BASELINE_GAP -
                peak_h - SPECTRUM_SEGMENTED_PEAK_GAP;
            peak.bg_color = lv_color_hex(SPECTRUM_GRADIENT[visual_index]);
            lv_area_t a = {
                x1,
                peak_y - SPECTRUM_SEGMENTED_PEAK_H + 1,
                x2,
                peak_y,
            };
            lv_draw_rect(layer, &peak, &a);
        }
    }
}

static void spectrum_widget_draw_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
        return;
    }

    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    if (layer == nullptr || obj == nullptr) {
        return;
    }

    if (g_style == SpectrumStyle::NeonRidge) {
        spectrum_draw_neon_ridge(layer, obj);
    } else if (g_style == SpectrumStyle::HorizontalMirror) {
        spectrum_draw_horizontal(layer, obj);
    } else {
        spectrum_draw_segmented_columns(layer, obj);
    }
}

static void spectrum_update_bars()
{
    // R.31：保留当前样式上一帧“真正画出来的几何”，raw bar 有细小变化但没有跨越
    // 当前样式的像素/分段量化边界时，不再无条件 invalidate 整个 spectrum widget。
    uint16_t old_visual[SPECTRUM_VISUAL_BAR_COUNT] = {};
    uint16_t old_peak[SPECTRUM_VISUAL_BAR_COUNT] = {};
    int16_t old_segmented[SPECTRUM_SEGMENTED_COLUMN_COUNT] = {};
    int16_t old_segmented_peak[SPECTRUM_SEGMENTED_COLUMN_COUNT] = {};
    int16_t old_ridge[SPECTRUM_RIDGE_POINT_COUNT] = {};
    if (g_style == SpectrumStyle::HorizontalMirror) {
        memcpy(old_visual, g_visual_height_cache, sizeof(old_visual));
        memcpy(old_peak, g_peak_height, sizeof(old_peak));
    } else if (g_style == SpectrumStyle::NeonRidge) {
        memcpy(old_ridge, g_ridge_height_cache, sizeof(old_ridge));
    } else {
        memcpy(old_segmented, g_segmented_height_cache, sizeof(old_segmented));
        memcpy(old_segmented_peak, g_segmented_peak_cache, sizeof(old_segmented_peak));
    }

    bool changed = false;
    for (uint8_t i = 0U; i < SPECTRUM_BAR_COUNT; ++i) {
        int32_t current = g_bar_height[i];
        const int32_t target = g_bar_target[i];
        if (target > current) {
            // P1.5.2R.3.1：瞬态 Attack 直接追目标，避免底鼓峰值被 100~300ms 平滑磨掉。
            current = target;
        } else {
            // 50ms 一帧时每帧收掉约一半差值：保留短暂尾巴，但不再长期悬在半高处。
            current = (current + target) / 2;
        }
        if (current < SPECTRUM_MIN_H) {
            current = SPECTRUM_MIN_H;
        }
        if (current > SPECTRUM_MAX_H) {
            current = SPECTRUM_MAX_H;
        }

        if (static_cast<uint16_t>(current) == g_bar_height[i]) {
            continue;
        }
        g_bar_height[i] = static_cast<uint16_t>(current);
        changed = true;
    }

    // R.31：16→24 插值每帧只算一次，Peak 与绘制共享。
    spectrum_rebuild_visual_cache();

    // P1.5.2R.4.1：Peak Dot 使用24根视觉柱自己的保持值。
    // 新峰值立即抬升；没有新峰值时每50ms下降3px，形成“降落点”。
    for (uint8_t i = 0U; i < SPECTRUM_VISUAL_BAR_COUNT; ++i) {
        const uint16_t visual_h = spectrum_visual_height(i);
        uint16_t peak_h = g_peak_height[i];
        if (visual_h >= peak_h) {
            peak_h = visual_h;
        } else if (peak_h > visual_h) {
            const uint16_t drop =
                peak_h - visual_h > SPECTRUM_PEAK_FALL_PX
                    ? SPECTRUM_PEAK_FALL_PX
                    : static_cast<uint16_t>(peak_h - visual_h);
            peak_h = static_cast<uint16_t>(peak_h - drop);
        }
        if (peak_h != g_peak_height[i]) {
            g_peak_height[i] = peak_h;
            changed = true;
        }
    }

    // R.31：Segmented / Neon 的比例换算也只在状态更新阶段执行一次；
    // DRAW_MAIN 只查缓存，避免每根线/矩形重复插值。
    spectrum_rebuild_style_geometry_cache();

    bool render_changed = changed;
    if (changed) {
        if (g_style == SpectrumStyle::HorizontalMirror) {
            render_changed =
                memcmp(old_visual, g_visual_height_cache, sizeof(old_visual)) != 0 ||
                memcmp(old_peak, g_peak_height, sizeof(old_peak)) != 0;
        } else if (g_style == SpectrumStyle::NeonRidge) {
            render_changed = memcmp(old_ridge, g_ridge_height_cache, sizeof(old_ridge)) != 0;
        } else {
            render_changed =
                memcmp(old_segmented, g_segmented_height_cache, sizeof(old_segmented)) != 0 ||
                memcmp(old_segmented_peak, g_segmented_peak_cache, sizeof(old_segmented_peak)) != 0;
        }
    }

    // 三种 FFT 样式继续共享一个自绘对象；只有当前样式实际像素几何变化才重绘。
    if (render_changed && g_spectrum_widget != nullptr) {
        lv_obj_invalidate(g_spectrum_widget);
    }
}

static void spectrum_root_click_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED || !g_visible) {
        return;
    }
    // R.35.2：样式切换只认真正 Tap。除了已成立的 Swipe 会 suppress 外，
    // 未达到页面手势阈值的普通拖动也必须拒绝，避免“拖了一下频谱却换样式”。
    if (gesture_router_should_suppress_click() ||
        !gesture_router_press_was_tap(SPECTRUM_STYLE_TAP_MAX_MOVE_PX)) {
        return;
    }

    const uint8_t next =
        (static_cast<uint8_t>(g_style) + 1U) % static_cast<uint8_t>(SpectrumStyle::Count);
    g_style = static_cast<SpectrumStyle>(next);
    // R.31：新样式第一次 DRAW_MAIN 前先用当前 bar 状态准备对应几何 cache。
    spectrum_rebuild_style_geometry_cache();
    spectrum_apply_style_layout();

    UI_PAGE_INTERACTION_LOGI("Tap切换频谱样式：style=%u %s",
        static_cast<unsigned>(g_style), spectrum_style_name(g_style));
}

static void spectrum_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!g_visible || g_root == nullptr || lv_obj_has_flag(g_root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    // 锁屏/AOD/熄屏动作菜单覆盖频谱时，50ms 频谱刷新完全让路；
    // 只冻结屏幕呈现，不影响后台音频播放。
    if (screen_action_menu_is_open()) {
        return;
    }

    AudioStateSnapshot audio = {};
    if (!audio_service_get_snapshot(&audio)) {
        return;
    }

    const uint32_t track_index = spectrum_current_track(audio);
    spectrum_refresh_header(track_index);

    if ((g_frame % SPECTRUM_LYRIC_POLL_FRAMES) == 0U) {
        spectrum_update_current_lyric(audio, track_index);
    }

    const bool playing = audio.state == AudioPlaybackState::Playing;
    // 防烧屏：真正停止/暂停时整组音柱收起；播放中保留最小高度防闪烁。
    g_idle_collapsed = !playing;
    if (playing) {
        AudioSpectrumSnapshot spectrum = {};
        if (
            audio_service_get_spectrum_snapshot(&spectrum) &&
            spectrum_snapshot_matches(spectrum, audio)
        ) {
            if (spectrum.revision != g_last_pcm_revision) {
                g_last_pcm_revision = spectrum.revision;
                spectrum_apply_pcm_snapshot(spectrum);
            }
        } else {
            // 新曲/Seek 切换窗口中没有匹配的 PCM Snapshot 时先回落，不显示上一首残影。
            g_last_pcm_revision = 0U;
            spectrum_set_idle_targets();
        }
    } else {
        g_last_pcm_revision = 0U;
        spectrum_set_idle_targets();
    }
    spectrum_update_bars();
    ++g_frame;

    if (g_time != nullptr) {
        const uint64_t displayed_second = audio.position_ms / 1000ULL;
        if (displayed_second != g_last_displayed_second) {
            g_last_displayed_second = displayed_second;
            char current[20] = {};
            spectrum_format_time(audio.position_ms, current, sizeof(current));
            lv_label_set_text(g_time, current);
        }
    }
}

// 延迟启动频谱定时器：让 LVGL 先完成页面切换首帧渲染，再开始频谱动画。
static void spectrum_delayed_start_cb(lv_timer_t *timer)
{
    if (timer != nullptr) {
        lv_timer_del(timer);
    }
    g_delayed_start_timer = nullptr;
    if (g_visible && g_timer != nullptr) {
        lv_timer_resume(g_timer);
    }
}
} // namespace

void spectrum_view_create(lv_obj_t *screen)
{
    if (screen == nullptr || g_root != nullptr) {
        return;
    }

    g_root = lv_obj_create(screen);
    ui_common_lock_object(g_root);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_size(g_root, 460, 460);
    lv_obj_set_style_radius(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_shadow_width(g_root, 0, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_root, spectrum_root_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);

    g_title = spectrum_create_label(g_root, "频谱", lv_color_hex(0xFFFFFF), 392, 34);
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 24);

    g_artist = spectrum_create_label(g_root, "", lv_color_hex(0x8995A6), 392, 30);
    lv_obj_align(g_artist, LV_ALIGN_TOP_MID, 0, 60);

    g_spectrum_widget = lv_obj_create(g_root);
    ui_common_lock_object(g_spectrum_widget);
    lv_obj_set_pos(g_spectrum_widget, SPECTRUM_AREA_LEFT, SPECTRUM_AREA_TOP);
    lv_obj_set_size(g_spectrum_widget, SPECTRUM_AREA_W, SPECTRUM_AREA_H);
    lv_obj_set_style_radius(g_spectrum_widget, 0, 0);
    lv_obj_set_style_bg_opa(g_spectrum_widget, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_spectrum_widget, 0, 0);
    lv_obj_set_style_shadow_width(g_spectrum_widget, 0, 0);
    lv_obj_set_style_pad_all(g_spectrum_widget, 0, 0);
    lv_obj_remove_flag(g_spectrum_widget, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_spectrum_widget, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_spectrum_widget, spectrum_widget_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

    for (uint8_t i = 0U; i < SPECTRUM_BAR_COUNT; ++i) {
        g_bar_height[i] = SPECTRUM_MIN_H;
        g_bar_target[i] = SPECTRUM_MIN_H;
    }
    for (uint8_t i = 0U; i < SPECTRUM_VISUAL_BAR_COUNT; ++i) {
        g_peak_height[i] = SPECTRUM_MIN_H;
    }
    spectrum_init_static_draw_cache();
    spectrum_rebuild_visual_cache();
    spectrum_rebuild_style_geometry_cache();

    g_lyric = spectrum_create_label(
        g_root, "", lv_color_hex(0xE8E8E8), SPECTRUM_LYRIC_W, SPECTRUM_LYRIC_H);
    lv_obj_set_pos(g_lyric, SPECTRUM_LYRIC_X, SPECTRUM_LYRIC_Y);
    lv_label_set_long_mode(g_lyric, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(g_lyric, SPECTRUM_LYRIC_LINE_SPACE, 0);

    g_time = spectrum_create_label(g_root, "0:00", lv_color_hex(0x7E8998), 180, 30);
    lv_obj_align(g_time, LV_ALIGN_BOTTOM_MID, 0, -38);

    spectrum_apply_style_layout();

    g_timer = lv_timer_create(spectrum_timer_cb, SPECTRUM_FRAME_MS, nullptr);
    if (g_timer != nullptr) {
        lv_timer_pause(g_timer);
    }

    UI_PAGE_BOOT_LOGI(
        "频谱页：3 styles，刷新=%ums，continuous-TE-bypass",
        static_cast<unsigned>(SPECTRUM_FRAME_MS));
}

void spectrum_view_open()
{
    if (g_root == nullptr || g_visible) {
        return;
    }

    g_visible = true;
    g_last_track = UINT32_MAX;
    g_frame = 0U;
    g_last_pcm_revision = 0U;
    g_last_displayed_second = UINT64_MAX;
    spectrum_clear_current_lyric(true);
    spectrum_set_idle_targets();
    g_idle_collapsed = true; // 打开页面先收起，等首个播放快照到达后再显示
    spectrum_apply_style_layout();
    for (uint8_t i = 0U; i < SPECTRUM_VISUAL_BAR_COUNT; ++i) {
        g_peak_height[i] = SPECTRUM_MIN_H;
    }
    spectrum_rebuild_visual_cache();
    spectrum_rebuild_style_geometry_cache();
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    screen_lock_simple_raise();

    AudioStateSnapshot audio = {};
    if (audio_service_get_snapshot(&audio)) {
        const uint32_t track_index = spectrum_current_track(audio);
        spectrum_refresh_header(track_index);
        spectrum_update_current_lyric(audio, track_index);
        if (audio.state == AudioPlaybackState::Playing) {
            AudioSpectrumSnapshot spectrum = {};
            if (
                audio_service_get_spectrum_snapshot(&spectrum) &&
                spectrum_snapshot_matches(spectrum, audio)
            ) {
                g_last_pcm_revision = spectrum.revision;
                spectrum_apply_pcm_snapshot(spectrum);
            }
        }
        char current[20] = {};
        spectrum_format_time(audio.position_ms, current, sizeof(current));
        lv_label_set_text(g_time, current);
        g_last_displayed_second = audio.position_ms / 1000ULL;
    }
    spectrum_update_bars();
    // 延迟 100ms 启动频谱定时器，让 LVGL 先完成页面切换首帧渲染，
    // 避免首帧渲染与频谱自绘叠加造成卡顿。
    if (g_timer != nullptr) {
        if (g_delayed_start_timer != nullptr) {
            lv_timer_del(g_delayed_start_timer);
        }
        g_delayed_start_timer = lv_timer_create(spectrum_delayed_start_cb, 100, nullptr);
        if (g_delayed_start_timer != nullptr) {
            lv_timer_set_repeat_count(g_delayed_start_timer, 1);
        }
    }
    audio_service_set_spectrum_enabled(true);
    UI_PAGE_INTERACTION_LOGI("打开频谱页：style=%u %s",
        static_cast<unsigned>(g_style), spectrum_style_name(g_style));
}

void spectrum_view_close()
{
    if (g_root == nullptr || !g_visible) {
        return;
    }

    g_visible = false;
    audio_service_set_spectrum_enabled(false);
    if (g_delayed_start_timer != nullptr) {
        lv_timer_del(g_delayed_start_timer);
        g_delayed_start_timer = nullptr;
    }
    if (g_timer != nullptr) {
        lv_timer_pause(g_timer);
    }
    spectrum_clear_current_lyric(true);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    UI_PAGE_INTERACTION_LOGI("关闭频谱页，返回主页");
}

bool spectrum_view_is_visible()
{
    return g_visible && g_root != nullptr && !lv_obj_has_flag(g_root, LV_OBJ_FLAG_HIDDEN);
}


const char *spectrum_view_current_style_name()
{
    return spectrum_style_name(g_style);
}
