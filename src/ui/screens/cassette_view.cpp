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
#include "font/font_manager.h"
#include "media_catalog_v2.h"
#include "media_library.h"
#include "player_state.h"
#include "lyrics/lyrics_service.h"
#include "ui_common.h"

static const char *TAG = "磁带视觉";

// Music 磁带视觉：CoverSurface + 静态外壳 + C2机械件 Sprite。音频链保持只读。
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

// C2.1：用户提供的机械件严格按最终 460x296 外壳像素坐标放置。
// 大轮 56x56 使用 6 帧（0/10/20/30/40/50°）；8 点小轮 44x44 只保留
// 2 个唯一相位（0/22.5°）。运行期仅移动 Sprite Strip，不做 LVGL rotate/scale。
static constexpr uint8_t kBigReelFrameCount = 6U;
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
static constexpr int64_t kMechanicsFramePeriodUs = 100000LL;  // 10 Hz，沿用主页现有100ms timer

// 大卷轴采用 Q16.16“帧相位”累积。磁带少时约 1.0 frame/tick，
// 磁带满时约 0.5 frame/tick；随播放进度连续插值，避免左右卷轴同步。
// 6 齿大轮每帧相差 10°，60° 后外观周期重复。
static constexpr uint32_t kPhaseOneFrameQ16 = 1U << 16U;
static constexpr uint32_t kBigReelSlowStepQ16 = kPhaseOneFrameQ16 / 2U;
static constexpr uint32_t kBigReelFastStepQ16 = kPhaseOneFrameQ16;

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
static lv_obj_t *g_shell_image = nullptr;
static lv_obj_t *g_tape_amount_image = nullptr;
static lv_obj_t *g_big_reel_viewports[2] = {};
static lv_obj_t *g_big_reel_strip_images[2] = {};
static lv_obj_t *g_small_roller_viewports[2] = {};
static lv_obj_t *g_small_roller_strip_images[2] = {};
static lv_obj_t *g_title_label = nullptr;
static lv_obj_t *g_artist_label = nullptr;
static lv_obj_t *g_current_lyric_label = nullptr;
static lv_obj_t *g_next_lyric_label = nullptr;
static bool g_active = false;
static bool g_controls_visible = false;

static uint32_t g_text_track = UINT32_MAX;
static uint32_t g_lyrics_requested_track = UINT32_MAX;
static uint32_t g_last_lyrics_revision = 0U;
static uint32_t g_last_lyrics_line = UINT32_MAX;

static uint8_t *g_shell_pixels = nullptr;
static lv_image_dsc_t g_shell_dsc = {};

static uint8_t *g_big_reel_pixels = nullptr;
static uint8_t *g_small_roller_pixels = nullptr;
static uint8_t *g_tape_amount_pixels = nullptr;
static lv_image_dsc_t g_big_reel_dsc = {};
static lv_image_dsc_t g_small_roller_dsc = {};
static lv_image_dsc_t g_tape_amount_dsc = {};
static bool g_mechanics_ready = false;
static uint32_t g_big_reel_phase_q16[2] = {};
static uint32_t g_small_roller_phase_q16 = 0U;
static int16_t g_last_tape_shift = INT16_MIN;
static int64_t g_last_mechanics_frame_us = 0LL;

static CoverSurfaceLease g_cover_lease = {};
static lv_image_dsc_t g_cover_dsc = {};
static uint32_t g_cover_generation = 0U;
static uint32_t g_cover_track = UINT32_MAX;


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

static void cassette_view_release_cover()
{
    if (g_cover_lease.slot_index != 0xFFU) {
        cover_surface_cache_release(&g_cover_lease);
    }
    g_cover_lease = {};
    g_cover_dsc = {};
    g_cover_generation = 0U;
    g_cover_track = UINT32_MAX;
    if (g_cover_image != nullptr) {
        lv_obj_add_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);
    }
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
    g_last_mechanics_frame_us = esp_timer_get_time();
    cassette_view_apply_mechanics_frames(0U, 0U, 0U);

    const size_t mechanics_bytes =
        static_cast<size_t>(g_big_reel_dsc.data_size) +
        static_cast<size_t>(g_small_roller_dsc.data_size) +
        static_cast<size_t>(g_tape_amount_dsc.data_size);
    ESP_LOGI(TAG,
        "机械件已准备：大轮=%uB(56x56x6) 小轮=%uB(44x44x2) 磁带量=%uB(159x41) total=%uB PSRAM",
        static_cast<unsigned>(g_big_reel_dsc.data_size),
        static_cast<unsigned>(g_small_roller_dsc.data_size),
        static_cast<unsigned>(g_tape_amount_dsc.data_size),
        static_cast<unsigned>(mechanics_bytes));
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

    int16_t shift = 0;
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
    if (!audio.ready || !same_track || total_ms == 0U) return kPhaseOneFrameQ16 / 2U;

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
    if (!g_mechanics_ready || !g_active) return;

    AudioStateSnapshot audio = {};
    if (!audio_service_get_snapshot(&audio)) return;
    cassette_view_update_tape_amount(audio);

    const bool moving =
        audio.state == AudioPlaybackState::Playing ||
        audio.state == AudioPlaybackState::Seeking;
    const int64_t now_us = esp_timer_get_time();
    if (!moving) {
        // Pause/Stopped 时机械运动冻结；恢复播放后从冻结相位继续。
        g_last_mechanics_frame_us = now_us;
        return;
    }

    if (g_last_mechanics_frame_us <= 0LL) {
        g_last_mechanics_frame_us = now_us;
        return;
    }
    int64_t elapsed_us = now_us - g_last_mechanics_frame_us;
    if (elapsed_us < kMechanicsFramePeriodUs) return;

    // C2.2：实机确认左右卷轴快慢映射与当前磁带量视觉相反。
    // 按当前磁带量方向修正为：
    // 0%  左边磁带少 -> 左快、右边磁带多 -> 右慢；
    // 100% 左边磁带多 -> 左慢、右边磁带少 -> 右快。
    // 速度仍按进度连续插值，两个大卷轴会平滑交换快慢。
    const uint32_t progress_q16 = cassette_view_progress_q16(audio);
    const uint32_t speed_span_q16 = kBigReelFastStepQ16 - kBigReelSlowStepQ16;
    const uint32_t speed_offset_q16 = static_cast<uint32_t>(
        (static_cast<uint64_t>(speed_span_q16) * progress_q16) / kPhaseOneFrameQ16);
    const uint32_t left_speed_q16 = kBigReelFastStepQ16 - speed_offset_q16;
    const uint32_t right_speed_q16 = kBigReelSlowStepQ16 + speed_offset_q16;
    const bool seeking = audio.state == AudioPlaybackState::Seeking;

    g_big_reel_phase_q16[0] = cassette_view_advance_phase_q16(
        g_big_reel_phase_q16[0], left_speed_q16, elapsed_us, kBigReelFrameCount, seeking);
    g_big_reel_phase_q16[1] = cassette_view_advance_phase_q16(
        g_big_reel_phase_q16[1], right_speed_q16, elapsed_us, kBigReelFrameCount, seeking);

    // 小滚轮保持固定线速度；2帧以100ms节拍交替即可。
    g_small_roller_phase_q16 = cassette_view_advance_phase_q16(
        g_small_roller_phase_q16, kPhaseOneFrameQ16, elapsed_us,
        kSmallRollerFrameCount, seeking);

    cassette_view_apply_mechanics_frames(
        static_cast<uint8_t>((g_big_reel_phase_q16[0] >> 16U) % kBigReelFrameCount),
        static_cast<uint8_t>((g_big_reel_phase_q16[1] >> 16U) % kBigReelFrameCount),
        static_cast<uint8_t>((g_small_roller_phase_q16 >> 16U) % kSmallRollerFrameCount));
    g_last_mechanics_frame_us = now_us;
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

    g_shell_pixels = native;
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

    ESP_LOGI(TAG, "磁带壳已准备：PNG=%uB RGB565A8(planar)=%uB stride=%u PSRAM",
        static_cast<unsigned>(png_size),
        static_cast<unsigned>(native_bytes),
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
    if (g_cover_lease.slot_index != 0xFFU &&
        g_cover_generation == generation && g_cover_track == track) {
        return true;
    }

    CoverSurfaceLease next = {};
    if (!cover_surface_cache_acquire(track, &next) || next.normal_rgb565 == nullptr ||
        next.width == 0U || next.height == 0U || next.data_size == 0U) {
        return false;
    }

    // 先 acquire 新 Surface，再释放旧 Surface，保持切歌视觉连续。
    CoverSurfaceLease old = g_cover_lease;
    g_cover_lease = next;
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
    if (g_cover_lease.width > 0U) {
        cover_scale = static_cast<uint32_t>(
            (static_cast<uint64_t>(kCoverBleedWidth) * kLvImageScaleNone +
             static_cast<uint64_t>(g_cover_lease.width) / 2U) /
            static_cast<uint64_t>(g_cover_lease.width));
        if (cover_scale == 0U) cover_scale = 1U;
    }
    lv_image_set_scale(g_cover_image, cover_scale);
    lv_image_set_antialias(g_cover_image, false);
    lv_obj_center(g_cover_image);
    lv_obj_remove_flag(g_cover_image, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "磁带封面已绑定：track=%lu source=%ux%u label=%dx%d bleed=%upx scale=%u/256",
        static_cast<unsigned long>(track),
        static_cast<unsigned>(g_cover_lease.width),
        static_cast<unsigned>(g_cover_lease.height),
        static_cast<int>(kLabelWidth),
        static_cast<int>(kLabelHeight),
        static_cast<unsigned>(kCoverBleedWidth),
        static_cast<unsigned>(cover_scale));

    if (old.slot_index != 0xFFU) {
        cover_surface_cache_release(&old);
    }
    return true;
}

esp_err_t cassette_view_create(lv_obj_t *parent)
{
    if (parent == nullptr) return ESP_ERR_INVALID_ARG;
    if (g_root != nullptr) return ESP_OK;

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
        kCassetteX + kTapeAmountBaseX,
        kCassetteY + kTapeAmountY);
    lv_obj_add_flag(g_tape_amount_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_tape_amount_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_tape_amount_image, LV_OBJ_FLAG_SCROLLABLE);

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
        cassette_view_set_mechanics_visible(mechanics_ok);
        cassette_view_update_track_text();
        cassette_view_apply_aux_visibility();
        cassette_view_update_mini_lyrics();
        cassette_view_update_mechanics();
        return true;
    }

    g_active = false;
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
    g_controls_visible = visible;
    cassette_view_apply_aux_visibility();
    if (g_active && !visible) {
        cassette_view_update();
    }
}

bool cassette_view_is_active()
{
    return g_active;
}
