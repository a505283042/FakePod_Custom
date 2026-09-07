#include "library_view.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "app_diag_config.h"
#include "esp_heap_caps.h"
#include "board_pins.h"
#include "device_settings.h"
#include "flac_decoder.h"
#include "font/font_manager.h"
#include "media_catalog_v2.h"
#include "media_groups_v2.h"
#include "media_library.h"
#include "player_control.h"
#include "player_home.h"
#include "player_playlist.h"
#include "player_state.h"
#include "search/search_key_builder.h"
#include "system/screen_lock_simple.h"
#include "ui_common.h"
#include "widgets/quick_index_keyboard.h"

static const char *TAG = "曲库界面";

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

// P1.3.5.4.1：460x460 方屏曲库，Direct Touch + 音频感知惯性 + 轻量常驻位置条。
// 顶部固定标题/搜索/分类指示器，下面恰好保留约五行单行列表。
static constexpr int32_t LIBRARY_LIST_X = 8;
static constexpr int32_t LIBRARY_LIST_Y = 92;
static constexpr int32_t LIBRARY_LIST_W = FAKEPOD_LCD_WIDTH - 16;
static constexpr int32_t LIBRARY_LIST_H = FAKEPOD_LCD_HEIGHT - LIBRARY_LIST_Y - 8;
static constexpr int32_t LIBRARY_ROW_X = 4;
static constexpr int32_t LIBRARY_ROW_W = LIBRARY_LIST_W - 8;
static constexpr int32_t LIBRARY_ROW_H = 62;
static constexpr int32_t LIBRARY_ROW_STEP = 70;
static constexpr uint32_t LIBRARY_VIRTUAL_ROWS = 7U; // 5 可见 + 上下缓冲

static constexpr int16_t LIBRARY_AXIS_LOCK_PX = 6;
static constexpr int16_t LIBRARY_LIST_DRAG_GUARD_PX = 5;
static constexpr int16_t LIBRARY_HORIZONTAL_TRIGGER_PX = 72;
static constexpr uint32_t LIBRARY_HORIZONTAL_MIN_SPEED = 45U;
static constexpr uint32_t LIBRARY_SUPPRESS_CLICK_MS = 320U;
static constexpr uint8_t LIBRARY_SEARCH_QUERY_MAX = 8U;
static constexpr size_t LIBRARY_SEARCH_KEY_MAX = 32U;

// P1.3.4.3：方屏 Header 左右按钮保持 20px 内收，并横向扩大触摸区；标题固定居中。
static constexpr int32_t LIBRARY_HEADER_BUTTON_Y = 11;
static constexpr int32_t LIBRARY_HEADER_BUTTON_W = 76;
static constexpr int32_t LIBRARY_HEADER_BUTTON_H = 46;
static constexpr int32_t LIBRARY_BACK_BUTTON_X = 20;
static constexpr int32_t LIBRARY_SEARCH_BUTTON_X = FAKEPOD_LCD_WIDTH - 20 - LIBRARY_HEADER_BUTTON_W;
static constexpr int32_t LIBRARY_TITLE_X = 110;
static constexpr int32_t LIBRARY_TITLE_W = FAKEPOD_LCD_WIDTH - LIBRARY_TITLE_X * 2;
// 目录范围标题保持居中，右侧只放一个极小层级图标，避免标题栏堆字。
static constexpr int32_t LIBRARY_SCOPE_TITLE_X = 124;
static constexpr int32_t LIBRARY_SCOPE_TITLE_W = 206;
static constexpr int32_t LIBRARY_SCOPE_ICON_X = 336;
static constexpr int32_t LIBRARY_SCOPE_ICON_Y = 21;
static constexpr int32_t LIBRARY_SCOPE_ICON_W = 22;
static constexpr int32_t LIBRARY_SCOPE_ICON_H = 20;

// P1.3.5.1：搜索模式保留 5 行结果，上方继续使用原 Header，下方固定 2x5 快速索引键盘。
static constexpr int32_t LIBRARY_SEARCH_LIST_Y = 68;
static constexpr int32_t LIBRARY_SEARCH_LIST_H = 250;
static constexpr int32_t LIBRARY_SEARCH_ROW_H = 44;
static constexpr int32_t LIBRARY_SEARCH_ROW_STEP = 50;
static constexpr int32_t LIBRARY_SEARCH_KEYBOARD_X = 16;
static constexpr int32_t LIBRARY_SEARCH_KEYBOARD_Y = 330;
static constexpr int32_t LIBRARY_SEARCH_KEYBOARD_W = FAKEPOD_LCD_WIDTH - 32;
static constexpr int32_t LIBRARY_SEARCH_KEYBOARD_H = 116;

// P1.3.5.4.1：直驱滚动保持不变；惯性只在音频余量安全时运行。
static constexpr uint32_t LIBRARY_INERTIA_PERIOD_MS = 16U;
static constexpr int32_t LIBRARY_INERTIA_START_MIN_PX_S = 180;
static constexpr int32_t LIBRARY_INERTIA_STOP_PX_S = 36;
static constexpr int32_t LIBRARY_INERTIA_MAX_PX_S = 3600;
static constexpr int32_t LIBRARY_INERTIA_DECAY_PERCENT = 92;
static constexpr uint32_t LIBRARY_INERTIA_DT_MAX_MS = 40U;

// P1.3.5.4.1：右侧常驻轻量位置条；滚动热路径只更新 thumb Y，不做样式/层级动画。
static constexpr int32_t LIBRARY_SCROLLBAR_W = 4;
static constexpr int32_t LIBRARY_SCROLLBAR_MARGIN_RIGHT = 5;
static constexpr int32_t LIBRARY_SCROLLBAR_MARGIN_Y = 6;
static constexpr int32_t LIBRARY_SCROLLBAR_MIN_THUMB_H = 28;
// P1.5R.1.2：列表直驱期间位置条只需约20Hz，避免每个触摸采样都多改一个LVGL对象。
static constexpr uint32_t LIBRARY_SCROLLBAR_TOUCH_UPDATE_MS = 48U;

// 四个顶层浏览分类与 PlayerListType 对齐，但 UI 浏览状态仍与播放上下文分离。
enum class LibraryBrowseMode : uint8_t
{
    AllTracks = 0,
    Artists = 1,
    Albums = 2,
    Decades = 3,
    GroupTracks = 4,
};

enum class LibraryRowAction : uint8_t
{
    None = 0,
    PlayAllTrack,
    PlayFolderTrack,
    OpenArtist,
    OpenAlbum,
    OpenDecade,
    PlayGroupTrack,
    SelectLevel1Folder,
    SelectLevel2Folder,
    SelectLevel2Parent,
    FolderUp,
};

enum class LibraryFolderView : uint8_t
{
    Tracks = 0,
    Level1Folders,
    Level2Folders,
    Level2Parents,
};

enum class LibraryGestureAxis : uint8_t
{
    None = 0,
    Horizontal,
    Vertical,
};

enum class LibraryPendingGesture : uint8_t
{
    None = 0,
    SwipeLeft,
    SwipeRight,
};

enum class LibrarySearchBucket : uint8_t
{
    ABC = 0,
    DEF = 1,
    GHI = 2,
    JKL = 3,
    MNO = 4,
    PQRS = 5,
    TUV = 6,
    WXYZ = 7,
    Other = 8,
    All = 9,
};

struct LibraryThemeColors
{
    uint32_t page_bg = 0x0D1016;
    uint32_t header = 0xF2F3F5;
    uint32_t accent = 0xF2F3F5;
    uint32_t row_bg = 0x151A21;
    uint32_t row_current_bg = 0x252C37;
};

struct LibraryRowBinding
{
    LibraryRowAction action = LibraryRowAction::None;
    PlayerListType group_type = PlayerListType::AllTracks;
    uint32_t generation = 0;
    uint32_t group_index = UINT32_MAX;
    uint32_t position = 0;
};

struct LibraryVirtualRow
{
    lv_obj_t *button = nullptr;
    lv_obj_t *accent = nullptr;
    lv_obj_t *label = nullptr;
    lv_obj_t *meta = nullptr;
    lv_obj_t *arrow = nullptr;
    LibraryRowBinding binding = {};
    uint32_t item_index = UINT32_MAX;
};

struct LibraryBrowseState
{
    LibraryBrowseMode mode = LibraryBrowseMode::AllTracks;
    PlayerListType detail_type = PlayerListType::AllTracks;
    uint32_t detail_group_index = UINT32_MAX;
    int32_t top_scroll_y[4] = {};
    int32_t detail_scroll_y = 0;
    int32_t parent_scroll_y = 0;
    LibraryFolderView folder_view = LibraryFolderView::Tracks;
    int32_t folder_scroll_y[3] = {};
    char level2_parent_path[PLAYER_FOLDER_PATH_MAX] = {};
};

struct LibraryGestureState
{
    bool pressed = false;
    bool ignore = false;
    int16_t start_x = 0;
    int16_t start_y = 0;
    int16_t last_x = 0;
    int16_t last_y = 0;
    uint32_t start_tick_ms = 0;
    LibraryGestureAxis axis = LibraryGestureAxis::None;
    LibraryPendingGesture pending = LibraryPendingGesture::None;
    bool async_scheduled = false;
    bool started_in_list = false;
    bool list_dragged = false;
    uint32_t last_sample_tick_ms = 0U;
    uint32_t last_motion_tick_ms = 0U;
    int32_t scroll_velocity_px_s = 0;
    uint32_t suppress_click_until = 0;
};

struct LibraryInertiaState
{
    lv_timer_t *timer = nullptr;
    bool active = false;
    int32_t velocity_px_s = 0;
    int32_t remainder_milli_px = 0;
    uint32_t last_tick_ms = 0U;
};

struct LibraryScrollbarState
{
    lv_obj_t *track = nullptr;
    lv_obj_t *thumb = nullptr;
    int32_t track_y = 0;
    int32_t track_h = 0;
    int32_t thumb_h = 0;
    int32_t travel = 0;
    int32_t max_scroll = 0;
    bool visible = false;
    uint32_t last_touch_update_tick_ms = 0U;
    int32_t last_thumb_y = 0;
    bool thumb_y_valid = false;
};

struct LibrarySearchKeyEntry
{
    uint32_t offset = 0U;
    uint16_t length = 0U;
};

struct LibrarySearchState
{
    bool active = false;
    uint8_t key_index = static_cast<uint8_t>(LibrarySearchBucket::All); // Decade search only.
    uint8_t query[LIBRARY_SEARCH_QUERY_MAX] = {};
    uint8_t query_length = 0U;
    int32_t parent_scroll_y = 0;
    int32_t scroll_y = 0;

    uint32_t *matches = nullptr;
    uint32_t match_count = 0;
    uint32_t capacity = 0;

    LibrarySearchKeyEntry *key_entries = nullptr;
    char *key_pool = nullptr;
    uint32_t key_count = 0U;
    size_t key_pool_size = 0U;
    uint32_t cache_generation = 0U;
    LibraryBrowseMode cache_mode = LibraryBrowseMode::AllTracks;
    PlayerListType cache_detail_type = PlayerListType::AllTracks;
    uint32_t cache_detail_group_index = UINT32_MAX;
    PlayerFolderScope cache_folder_scope = PlayerFolderScope::All;
    uint32_t cache_folder_context_id = 0U;
};

static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_header_title = nullptr;
static lv_obj_t *g_scope_icon = nullptr;
static lv_obj_t *g_scope_icon_back = nullptr;
static lv_obj_t *g_scope_icon_back_tab = nullptr;
static lv_obj_t *g_scope_icon_front = nullptr;
static lv_obj_t *g_scope_icon_front_tab = nullptr;
static lv_obj_t *g_back_button = nullptr;
static lv_obj_t *g_search_button = nullptr;
static lv_obj_t *g_indicator[4] = {};
static lv_obj_t *g_list_host = nullptr;
static int32_t g_manual_scroll_y = 0;
static lv_obj_t *g_hint = nullptr;
static LibraryVirtualRow g_rows[LIBRARY_VIRTUAL_ROWS] = {};
static LibraryBrowseState g_state = {};
static LibraryGestureState g_gesture = {};
static LibrarySearchState g_search = {};
static QuickIndexKeyboard g_search_keyboard = {};
static LibraryInertiaState g_inertia = {};
static LibraryScrollbarState g_scrollbar = {};

static void library_view_render(bool preserve_scroll = true);
static void library_view_refresh_virtual_rows(bool force = false);
static const char *library_search_scope_name();
static void library_view_inertia_stop(bool store_position);
static void library_view_scrollbar_rebuild_geometry();
static void library_view_scrollbar_update_position();
static bool library_view_inertia_audio_safe();
static LibraryBrowseMode library_view_category_mode_for_detail();
static void library_view_return_to_parent_category();

static int32_t library_abs(int32_t value)
{
    return value < 0 ? -value : value;
}

static int32_t library_clamp_i32(int32_t value, int32_t lo, int32_t hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static bool library_speed_ok(int32_t distance_px, uint32_t elapsed_ms, uint32_t min_speed_px_s)
{
    if (distance_px <= 0) {
        return false;
    }
    if (elapsed_ms == 0U) {
        return true;
    }
    return static_cast<uint64_t>(distance_px) * 1000ULL >=
        static_cast<uint64_t>(min_speed_px_s) * elapsed_ms;
}

static bool library_click_suppressed()
{
    const uint32_t now = static_cast<uint32_t>(lv_tick_get());
    return static_cast<int32_t>(g_gesture.suppress_click_until - now) > 0;
}

static LibraryThemeColors library_view_theme_colors()
{
    // 目录列表只用轻微色相区分层级：一级冷青、二级暖琥珀。
    // 总列表继续沿用原有“歌曲/歌手/专辑/年代”主题，不受影响。
    const PlayerFolderScope folder_scope = player_control_get_folder_scope();
    if (folder_scope == PlayerFolderScope::Level1) {
        return LibraryThemeColors{0x091216, 0x79D8E2, 0x58C8D5, 0x111C21, 0x18343B};
    }
    if (folder_scope == PlayerFolderScope::Level2) {
        return LibraryThemeColors{0x151008, 0xE7BC69, 0xD9A84F, 0x1D1810, 0x382B18};
    }

    const LibraryBrowseMode parent = library_view_category_mode_for_detail();
    const bool detail = g_state.mode == LibraryBrowseMode::GroupTracks;

    switch (parent) {
        case LibraryBrowseMode::Artists:
            // 歌手：冷蓝。详情页保持同一色相，只降低明度并加深页面底色。
            return detail
                ? LibraryThemeColors{0x090D12, 0x6F96BA, 0x668CB1, 0x101820, 0x1B2B3B}
                : LibraryThemeColors{0x0D1116, 0x8BB9E3, 0x7DAEDB, 0x131B24, 0x203449};
        case LibraryBrowseMode::Albums:
            // 专辑：低饱和紫。详情页继承色相，不切换成另一套主题。
            return detail
                ? LibraryThemeColors{0x0D0A12, 0x8E79B1, 0x806DA5, 0x15111B, 0x2B2138}
                : LibraryThemeColors{0x110E16, 0xB5A0DD, 0xA28ACC, 0x1A1621, 0x332744};
        case LibraryBrowseMode::Decades:
            // 年代：暖琥珀。详情页只做明度变化，保持父级视觉归属。
            return detail
                ? LibraryThemeColors{0x100D07, 0xB58C55, 0xA67E4C, 0x17130D, 0x342619}
                : LibraryThemeColors{0x151109, 0xDBB072, 0xC99B5D, 0x1D1811, 0x40301C};
        case LibraryBrowseMode::AllTracks:
        case LibraryBrowseMode::GroupTracks:
            return {};
    }
    return {};
}

static lv_obj_t *library_view_create_label(
    lv_obj_t *parent,
    const char *text,
    lv_color_t color,
    const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    ui_common_lock_object(label);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font != nullptr ? font : font_manager_get_ui_font(), 0);
    return label;
}

static lv_obj_t *library_view_create_icon_button(
    lv_obj_t *parent,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const char *symbol)
{
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x171C24), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);

    lv_obj_t *label = library_view_create_label(button, symbol, lv_color_hex(0xE9ECF1), lv_font_default());
    lv_obj_center(label);
    return button;
}

static lv_obj_t *library_view_create_search_button(
    lv_obj_t *parent,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height)
{
    // LVGL 9.2 默认 Symbol 集没有放大镜字符，因此用 LVGL 基础对象直接画图标，
    // 仍然不依赖正文中文字体，也不引入任何外部图片资源。
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x171C24), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);

    lv_obj_t *ring = lv_obj_create(button);
    ui_common_lock_object(ring);
    const int32_t ring_x = width / 2 - 11;
    const int32_t ring_y = height / 2 - 12;
    lv_obj_set_pos(ring, ring_x, ring_y);
    lv_obj_set_size(ring, 17, 17);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xE9ECF1), 0);
    lv_obj_set_style_border_width(ring, 2, 0);

    lv_obj_t *handle = lv_obj_create(button);
    ui_common_lock_object(handle);
    lv_obj_set_pos(handle, ring_x + 16, ring_y + 15);
    lv_obj_set_size(handle, 2, 10);
    lv_obj_set_style_radius(handle, 1, 0);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0xE9ECF1), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_set_style_transform_pivot_x(handle, 1, 0);
    lv_obj_set_style_transform_pivot_y(handle, 1, 0);
    lv_obj_set_style_transform_rotation(handle, -450, 0);
    return button;
}

static LibraryBrowseMode library_view_category_mode_for_detail()
{
    if (g_state.mode != LibraryBrowseMode::GroupTracks) {
        return g_state.mode;
    }
    switch (g_state.detail_type) {
        case PlayerListType::Artist: return LibraryBrowseMode::Artists;
        case PlayerListType::Album: return LibraryBrowseMode::Albums;
        case PlayerListType::Decade: return LibraryBrowseMode::Decades;
        case PlayerListType::AllTracks: return LibraryBrowseMode::AllTracks;
    }
    return LibraryBrowseMode::AllTracks;
}

static bool library_view_folder_scope_active()
{
    if (player_control_get_folder_scope() == PlayerFolderScope::All) {
        return false;
    }
    PlayerFolderQueueSnapshot queue = {};
    return player_state_get_folder_queue_snapshot(&queue) &&
        queue.ready && queue.effective_scope != PlayerFolderScope::All;
}

static bool library_view_folder_browser_active()
{
    return library_view_folder_scope_active() && g_state.folder_view != LibraryFolderView::Tracks;
}

static const char *library_folder_leaf_in_place(char *path)
{
    if (path == nullptr || path[0] == '\0') return nullptr;
    size_t length = strlen(path);
    while (length > 0U && path[length - 1U] == '/') {
        path[--length] = '\0';
    }
    if (length == 0U) return nullptr;
    char *slash = strrchr(path, '/');
    return slash != nullptr ? slash + 1U : path;
}

static bool library_folder_level2_parent(
    const char *level2_path, char *buffer, size_t buffer_size)
{
    if (level2_path == nullptr || level2_path[0] == '\0' ||
        buffer == nullptr || buffer_size == 0U || strlen(level2_path) + 1U > buffer_size) {
        return false;
    }
    snprintf(buffer, buffer_size, "%s", level2_path);
    size_t length = strlen(buffer);
    while (length > 0U && buffer[length - 1U] == '/') {
        buffer[--length] = '\0';
    }
    char *slash = strrchr(buffer, '/');
    if (slash == nullptr || slash == buffer) return false;
    slash[1] = '\0';
    return player_playlist_folder_selection_available(
        PlayerFolderScope::Level1, buffer, nullptr);
}

static bool library_view_resolve_level2_parent()
{
    if (g_state.level2_parent_path[0] != '\0' &&
        player_playlist_folder_selection_available(
            PlayerFolderScope::Level1, g_state.level2_parent_path, nullptr)) {
        return true;
    }

    char level2_path[PLAYER_FOLDER_PATH_MAX] = {};
    if (player_control_copy_folder_selection(
            PlayerFolderScope::Level2, level2_path, sizeof(level2_path)) &&
        library_folder_level2_parent(
            level2_path, g_state.level2_parent_path, sizeof(g_state.level2_parent_path))) {
        return true;
    }

    char level1_path[PLAYER_FOLDER_PATH_MAX] = {};
    if (player_control_copy_folder_selection(
            PlayerFolderScope::Level1, level1_path, sizeof(level1_path)) &&
        player_playlist_folder_selection_available(
            PlayerFolderScope::Level1, level1_path, nullptr)) {
        snprintf(g_state.level2_parent_path, sizeof(g_state.level2_parent_path), "%s", level1_path);
        return true;
    }

    return player_playlist_copy_folder_option_at(
        PlayerFolderScope::Level1,
        nullptr,
        0U,
        g_state.level2_parent_path,
        sizeof(g_state.level2_parent_path),
        nullptr);
}

// 二级列表“上一级”只展示真正拥有二级音乐文件夹的一级目录，
// 避免把没有可进入二级列表的一级目录也堆在这里。
static size_t library_view_level2_parent_count()
{
    const size_t level1_count =
        player_playlist_get_folder_option_count(PlayerFolderScope::Level1, nullptr);
    size_t visible_count = 0U;
    for (size_t level1_pos = 0U; level1_pos < level1_count; ++level1_pos) {
        char path[PLAYER_FOLDER_PATH_MAX] = {};
        if (!player_playlist_copy_folder_option_at(
                PlayerFolderScope::Level1, nullptr, level1_pos,
                path, sizeof(path), nullptr)) {
            continue;
        }
        if (player_playlist_get_folder_option_count(PlayerFolderScope::Level2, path) > 0U) {
            ++visible_count;
        }
    }
    return visible_count;
}

static bool library_view_level2_parent_at(
    size_t visible_position,
    size_t *out_level1_position,
    char *out_path,
    size_t out_path_size,
    uint32_t *out_child_count)
{
    if (out_level1_position == nullptr || out_path == nullptr || out_path_size == 0U) {
        return false;
    }
    out_path[0] = '\0';
    const size_t level1_count =
        player_playlist_get_folder_option_count(PlayerFolderScope::Level1, nullptr);
    size_t visible_index = 0U;
    for (size_t level1_pos = 0U; level1_pos < level1_count; ++level1_pos) {
        char path[PLAYER_FOLDER_PATH_MAX] = {};
        if (!player_playlist_copy_folder_option_at(
                PlayerFolderScope::Level1, nullptr, level1_pos,
                path, sizeof(path), nullptr)) {
            continue;
        }
        const size_t children =
            player_playlist_get_folder_option_count(PlayerFolderScope::Level2, path);
        if (children == 0U) continue;
        if (visible_index++ != visible_position) continue;
        if (strlen(path) + 1U > out_path_size) return false;
        snprintf(out_path, out_path_size, "%s", path);
        *out_level1_position = level1_pos;
        if (out_child_count != nullptr) {
            *out_child_count = static_cast<uint32_t>(children);
        }
        return true;
    }
    return false;
}

static PlayerFolderScope library_folder_scope_from_setting(DeviceMusicListScope scope)
{
    switch (scope) {
        case DeviceMusicListScope::All: return PlayerFolderScope::All;
        case DeviceMusicListScope::Level1: return PlayerFolderScope::Level1;
        case DeviceMusicListScope::Level2: return PlayerFolderScope::Level2;
    }
    return PlayerFolderScope::All;
}

static bool library_view_persist_folder_selection(
    PlayerFolderScope scope, const char *folder_path)
{
    if ((scope != PlayerFolderScope::Level1 && scope != PlayerFolderScope::Level2) ||
        folder_path == nullptr || folder_path[0] == '\0') {
        return false;
    }

    DeviceMusicListSelection previous = {};
    if (!device_settings_get_music_list_selection(&previous)) return false;
    const PlayerFolderScope previous_runtime_scope = player_control_get_folder_scope();

    if (!player_control_set_folder_selection(scope, folder_path) ||
        !player_control_set_folder_scope(scope)) {
        return false;
    }

    const char *level1_path = scope == PlayerFolderScope::Level1
        ? folder_path : previous.level1_path;
    const char *level2_path = scope == PlayerFolderScope::Level2
        ? folder_path : previous.level2_path;
    const DeviceMusicListScope setting_scope = scope == PlayerFolderScope::Level1
        ? DeviceMusicListScope::Level1 : DeviceMusicListScope::Level2;
    const esp_err_t ret = device_settings_set_music_list_selection(
        setting_scope, level1_path, level2_path);
    if (ret == ESP_OK) {
        return true;
    }

    if (previous.level1_path[0] != '\0') {
        (void)player_control_set_folder_selection(PlayerFolderScope::Level1, previous.level1_path);
    }
    if (previous.level2_path[0] != '\0') {
        (void)player_control_set_folder_selection(PlayerFolderScope::Level2, previous.level2_path);
    }
    (void)player_control_set_folder_scope(
        previous_runtime_scope != PlayerFolderScope::All
            ? previous_runtime_scope
            : library_folder_scope_from_setting(previous.scope));
    ESP_LOGW(TAG, "保存主页文件夹选择失败：%s", esp_err_to_name(ret));
    return false;
}

static bool library_view_folder_queue_snapshot(PlayerFolderQueueSnapshot *out_snapshot)
{
    return out_snapshot != nullptr &&
        player_state_get_folder_queue_snapshot(out_snapshot) &&
        out_snapshot->ready;
}

static bool library_view_top_track_index(uint32_t position, uint32_t *out_track_index)
{
    if (out_track_index == nullptr) {
        return false;
    }
    if (!library_view_folder_scope_active()) {
        if (position >= media_library_get_count()) {
            return false;
        }
        *out_track_index = position;
        return true;
    }

    size_t track_index = 0U;
    if (!player_playlist_get_folder_queue_track_index_at_position(position, &track_index) ||
        track_index > UINT32_MAX) {
        return false;
    }
    *out_track_index = static_cast<uint32_t>(track_index);
    return true;
}

static uint32_t library_view_top_level_count(LibraryBrowseMode mode)
{
    switch (mode) {
        case LibraryBrowseMode::AllTracks:
            if (library_view_folder_scope_active()) {
                PlayerFolderQueueSnapshot queue = {};
                return library_view_folder_queue_snapshot(&queue) ? queue.track_count : 0U;
            }
            return static_cast<uint32_t>(media_library_get_count());
        case LibraryBrowseMode::Artists:
            return static_cast<uint32_t>(media_groups_v2_artist_count());
        case LibraryBrowseMode::Albums:
            return static_cast<uint32_t>(media_groups_v2_album_count());
        case LibraryBrowseMode::Decades:
            return static_cast<uint32_t>(media_groups_v2_decade_count());
        case LibraryBrowseMode::GroupTracks:
            break;
    }
    return 0U;
}

static bool library_view_get_detail_identity(
    const char **out_title,
    uint32_t *out_track_count,
    uint32_t *out_generation)
{
    if (g_state.mode != LibraryBrowseMode::GroupTracks) {
        return false;
    }

    switch (g_state.detail_type) {
        case PlayerListType::Artist:
        {
            MediaArtistGroupViewV2 view = {};
            if (!media_groups_v2_get_artist(g_state.detail_group_index, &view)) {
                return false;
            }
            if (out_title != nullptr) *out_title = view.name;
            if (out_track_count != nullptr) *out_track_count = view.track_count;
            if (out_generation != nullptr) *out_generation = view.generation;
            return true;
        }
        case PlayerListType::Album:
        {
            MediaAlbumGroupViewV2 view = {};
            if (!media_groups_v2_get_album(g_state.detail_group_index, &view)) {
                return false;
            }
            if (out_title != nullptr) *out_title = view.title;
            if (out_track_count != nullptr) *out_track_count = view.track_count;
            if (out_generation != nullptr) *out_generation = view.generation;
            return true;
        }
        case PlayerListType::Decade:
        {
            MediaDecadeGroupViewV2 view = {};
            if (!media_groups_v2_get_decade(g_state.detail_group_index, &view)) {
                return false;
            }
            static char decade_title[32] = {};
            if (view.unknown) {
                snprintf(decade_title, sizeof(decade_title), "未知年代");
            } else {
                snprintf(decade_title, sizeof(decade_title), "%u年代", static_cast<unsigned>(view.decade_start));
            }
            if (out_title != nullptr) *out_title = decade_title;
            if (out_track_count != nullptr) *out_track_count = view.track_count;
            if (out_generation != nullptr) *out_generation = view.generation;
            return true;
        }
        case PlayerListType::AllTracks:
            break;
    }
    return false;
}

static bool library_view_detail_track_index(uint32_t position, uint32_t *out_track_index, uint32_t *out_generation)
{
    if (out_track_index == nullptr || g_state.mode != LibraryBrowseMode::GroupTracks) {
        return false;
    }

    switch (g_state.detail_type) {
        case PlayerListType::Artist:
        {
            MediaArtistGroupViewV2 view = {};
            if (!media_groups_v2_get_artist(g_state.detail_group_index, &view) ||
                view.track_indices == nullptr || position >= view.track_count) {
                return false;
            }
            *out_track_index = view.track_indices[position];
            if (out_generation != nullptr) *out_generation = view.generation;
            return true;
        }
        case PlayerListType::Album:
        {
            MediaAlbumGroupViewV2 view = {};
            if (!media_groups_v2_get_album(g_state.detail_group_index, &view) ||
                view.track_indices == nullptr || position >= view.track_count) {
                return false;
            }
            *out_track_index = view.track_indices[position];
            if (out_generation != nullptr) *out_generation = view.generation;
            return true;
        }
        case PlayerListType::Decade:
        {
            MediaDecadeGroupViewV2 view = {};
            if (!media_groups_v2_get_decade(g_state.detail_group_index, &view) ||
                view.track_indices == nullptr || position >= view.track_count) {
                return false;
            }
            *out_track_index = view.track_indices[position];
            if (out_generation != nullptr) *out_generation = view.generation;
            return true;
        }
        case PlayerListType::AllTracks:
            break;
    }
    return false;
}

static int32_t library_view_list_y()
{
    return g_search.active ? LIBRARY_SEARCH_LIST_Y : LIBRARY_LIST_Y;
}

static int32_t library_view_list_h()
{
    return g_search.active ? LIBRARY_SEARCH_LIST_H : LIBRARY_LIST_H;
}

static int32_t library_view_row_h()
{
    return g_search.active ? LIBRARY_SEARCH_ROW_H : LIBRARY_ROW_H;
}

static int32_t library_view_row_step()
{
    return g_search.active ? LIBRARY_SEARCH_ROW_STEP : LIBRARY_ROW_STEP;
}

static uint32_t library_view_source_item_count()
{
    if (library_view_folder_browser_active()) {
        switch (g_state.folder_view) {
            case LibraryFolderView::Level1Folders:
                return static_cast<uint32_t>(
                    player_playlist_get_folder_option_count(PlayerFolderScope::Level1, nullptr));
            case LibraryFolderView::Level2Parents:
                return static_cast<uint32_t>(library_view_level2_parent_count());
            case LibraryFolderView::Level2Folders:
                if (!library_view_resolve_level2_parent()) return 1U;
                return static_cast<uint32_t>(
                    1U + player_playlist_get_folder_option_count(
                        PlayerFolderScope::Level2, g_state.level2_parent_path));
            case LibraryFolderView::Tracks:
                break;
        }
    }
    if (g_state.mode != LibraryBrowseMode::GroupTracks) {
        return library_view_top_level_count(g_state.mode);
    }
    uint32_t track_count = 0U;
    return library_view_get_detail_identity(nullptr, &track_count, nullptr) ? track_count : 0U;
}

static const char *library_search_source_text(uint32_t source_index)
{
    if (g_state.mode == LibraryBrowseMode::GroupTracks) {
        uint32_t track_index = UINT32_MAX;
        if (!library_view_detail_track_index(source_index, &track_index, nullptr)) {
            return nullptr;
        }
        MediaTrackViewV2 track = {};
        if (!media_catalog_v2_get_track_view(track_index, &track)) {
            return nullptr;
        }
        return track.title != nullptr && track.title[0] != '\0' ? track.title : track.path;
    }

    if (g_state.mode == LibraryBrowseMode::AllTracks) {
        uint32_t track_index = UINT32_MAX;
        if (!library_view_top_track_index(source_index, &track_index)) {
            return nullptr;
        }
        MediaTrackViewV2 track = {};
        if (!media_catalog_v2_get_track_view(track_index, &track)) {
            return nullptr;
        }
        return track.title != nullptr && track.title[0] != '\0' ? track.title : track.path;
    }

    if (g_state.mode == LibraryBrowseMode::Artists) {
        MediaArtistGroupViewV2 view = {};
        return media_groups_v2_get_artist(source_index, &view) ? view.name : nullptr;
    }

    if (g_state.mode == LibraryBrowseMode::Albums) {
        MediaAlbumGroupViewV2 view = {};
        return media_groups_v2_get_album(source_index, &view) ? view.title : nullptr;
    }

    return nullptr;
}

static uint8_t library_search_bucket_for_initial(char initial)
{
    uint8_t c = static_cast<uint8_t>(initial);
    if (c >= 'a' && c <= 'z') {
        c = static_cast<uint8_t>(c - ('a' - 'A'));
    }
    if (c >= 'A' && c <= 'C') return static_cast<uint8_t>(LibrarySearchBucket::ABC);
    if (c >= 'D' && c <= 'F') return static_cast<uint8_t>(LibrarySearchBucket::DEF);
    if (c >= 'G' && c <= 'I') return static_cast<uint8_t>(LibrarySearchBucket::GHI);
    if (c >= 'J' && c <= 'L') return static_cast<uint8_t>(LibrarySearchBucket::JKL);
    if (c >= 'M' && c <= 'O') return static_cast<uint8_t>(LibrarySearchBucket::MNO);
    if (c >= 'P' && c <= 'S') return static_cast<uint8_t>(LibrarySearchBucket::PQRS);
    if (c >= 'T' && c <= 'V') return static_cast<uint8_t>(LibrarySearchBucket::TUV);
    if (c >= 'W' && c <= 'Z') return static_cast<uint8_t>(LibrarySearchBucket::WXYZ);
    return static_cast<uint8_t>(LibrarySearchBucket::Other);
}

static bool library_search_ensure_capacity(uint32_t required)
{
    if (required <= g_search.capacity) {
        return true;
    }

    const size_t bytes = static_cast<size_t>(required) * sizeof(uint32_t);
    void *memory = heap_caps_realloc(
        g_search.matches,
        bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        memory = heap_caps_realloc(g_search.matches, bytes, MALLOC_CAP_8BIT);
    }
    if (memory == nullptr) {
        ESP_LOGE(TAG, "快速索引结果表申请失败：%lu项 / %uB",
            static_cast<unsigned long>(required), static_cast<unsigned>(bytes));
        return false;
    }

    g_search.matches = static_cast<uint32_t *>(memory);
    g_search.capacity = required;
    return true;
}

static void *library_search_alloc(size_t bytes)
{
    if (bytes == 0U) {
        return nullptr;
    }
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        memory = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return memory;
}

static void library_search_folder_cache_identity(
    PlayerFolderScope *out_scope,
    uint32_t *out_context_id)
{
    PlayerFolderScope scope = player_control_get_folder_scope();
    uint32_t context_id = 0U;
    if (scope != PlayerFolderScope::All) {
        PlayerFolderQueueSnapshot queue = {};
        if (library_view_folder_queue_snapshot(&queue)) {
            context_id = queue.context_id;
        }
    }
    if (out_scope != nullptr) *out_scope = scope;
    if (out_context_id != nullptr) *out_context_id = context_id;
}

static bool library_search_cache_signature_matches(uint32_t source_count)
{
    PlayerFolderScope folder_scope = PlayerFolderScope::All;
    uint32_t folder_context_id = 0U;
    library_search_folder_cache_identity(&folder_scope, &folder_context_id);

    return g_search.key_entries != nullptr && g_search.key_pool != nullptr &&
        g_search.key_count == source_count &&
        g_search.cache_generation == media_catalog_v2_generation() &&
        g_search.cache_mode == g_state.mode &&
        g_search.cache_detail_type == g_state.detail_type &&
        g_search.cache_detail_group_index == g_state.detail_group_index &&
        g_search.cache_folder_scope == folder_scope &&
        g_search.cache_folder_context_id == folder_context_id;
}

static bool library_search_build_key_cache()
{
    if (g_state.mode == LibraryBrowseMode::Decades) {
        return true;
    }

    const uint32_t source_count = library_view_source_item_count();
    if (library_search_cache_signature_matches(source_count)) {
        return true;
    }

    const uint32_t started_ms = static_cast<uint32_t>(lv_tick_get());
    size_t pool_bytes = 0U;
    char key[LIBRARY_SEARCH_KEY_MAX] = {};
    for (uint32_t source_index = 0U; source_index < source_count; ++source_index) {
        const char *text = library_search_source_text(source_index);
        const size_t length = search_key_build_initials(text, key, sizeof(key));
        pool_bytes += (length > 0U ? length : 1U) + 1U;
    }

    const size_t entry_bytes = static_cast<size_t>(source_count) * sizeof(LibrarySearchKeyEntry);
    auto *new_entries = static_cast<LibrarySearchKeyEntry *>(library_search_alloc(entry_bytes));
    auto *new_pool = static_cast<char *>(library_search_alloc(pool_bytes > 0U ? pool_bytes : 1U));
    if ((source_count > 0U && new_entries == nullptr) || new_pool == nullptr) {
        if (new_entries != nullptr) heap_caps_free(new_entries);
        if (new_pool != nullptr) heap_caps_free(new_pool);
        ESP_LOGE(TAG, "多首字母SearchKey缓存申请失败：entries=%uB pool=%uB",
            static_cast<unsigned>(entry_bytes), static_cast<unsigned>(pool_bytes));
        return false;
    }

    size_t cursor = 0U;
    for (uint32_t source_index = 0U; source_index < source_count; ++source_index) {
        const char *text = library_search_source_text(source_index);
        size_t length = search_key_build_initials(text, key, sizeof(key));
        if (length == 0U) {
            key[0] = '#';
            key[1] = '\0';
            length = 1U;
        }
        new_entries[source_index].offset = static_cast<uint32_t>(cursor);
        new_entries[source_index].length = static_cast<uint16_t>(length);
        for (size_t i = 0U; i <= length; ++i) {
            new_pool[cursor + i] = key[i];
        }
        cursor += length + 1U;
    }

    if (g_search.key_entries != nullptr) heap_caps_free(g_search.key_entries);
    if (g_search.key_pool != nullptr) heap_caps_free(g_search.key_pool);
    g_search.key_entries = new_entries;
    g_search.key_pool = new_pool;
    g_search.key_count = source_count;
    g_search.key_pool_size = cursor;
    g_search.cache_generation = media_catalog_v2_generation();
    g_search.cache_mode = g_state.mode;
    g_search.cache_detail_type = g_state.detail_type;
    g_search.cache_detail_group_index = g_state.detail_group_index;
    library_search_folder_cache_identity(
        &g_search.cache_folder_scope,
        &g_search.cache_folder_context_id);

    ESP_LOGI(TAG,
        "多首字母SearchKey缓存完成：scope=%s rows=%lu entries=%uB pool=%uB time=%lums",
        library_search_scope_name(),
        static_cast<unsigned long>(source_count),
        static_cast<unsigned>(entry_bytes),
        static_cast<unsigned>(cursor),
        static_cast<unsigned long>(static_cast<uint32_t>(lv_tick_get()) - started_ms));
    return true;
}

static const char *library_search_key_for_source(uint32_t source_index, char scratch[LIBRARY_SEARCH_KEY_MAX])
{
    const uint32_t source_count = library_view_source_item_count();
    if (library_search_cache_signature_matches(source_count) && source_index < g_search.key_count) {
        const LibrarySearchKeyEntry &entry = g_search.key_entries[source_index];
        if (entry.offset < g_search.key_pool_size) {
            return g_search.key_pool + entry.offset;
        }
    }

    const char *text = library_search_source_text(source_index);
    const size_t length = search_key_build_initials(text, scratch, LIBRARY_SEARCH_KEY_MAX);
    if (length == 0U) {
        scratch[0] = '#';
        scratch[1] = '\0';
    }
    return scratch;
}

static bool library_search_filter_active()
{
    if (!g_search.active) {
        return false;
    }
    if (g_state.mode == LibraryBrowseMode::Decades) {
        return g_search.key_index != static_cast<uint8_t>(LibrarySearchBucket::All);
    }
    return g_search.query_length > 0U;
}

static bool library_search_source_matches(uint32_t source_index)
{
    if (g_state.mode == LibraryBrowseMode::Decades) {
        const uint8_t key_index = g_search.key_index;
        if (key_index == static_cast<uint8_t>(LibrarySearchBucket::All)) {
            return true;
        }
        MediaDecadeGroupViewV2 view = {};
        if (!media_groups_v2_get_decade(source_index, &view)) {
            return false;
        }
        if (key_index < 8U) {
            const uint16_t decade = static_cast<uint16_t>(1950U + static_cast<uint16_t>(key_index) * 10U);
            return !view.unknown && view.decade_start == decade;
        }
        return key_index == 8U && view.unknown;
    }

    if (g_search.query_length == 0U) {
        return true;
    }

    char scratch[LIBRARY_SEARCH_KEY_MAX] = {};
    const char *key = library_search_key_for_source(source_index, scratch);
    if (key == nullptr) {
        return false;
    }
    for (uint8_t i = 0U; i < g_search.query_length; ++i) {
        if (key[i] == '\0' || library_search_bucket_for_initial(key[i]) != g_search.query[i]) {
            return false;
        }
    }
    return true;
}

static void library_search_rebuild_matches()
{
    if (!g_search.active) {
        return;
    }

    g_search.scroll_y = 0;
    const uint32_t source_count = library_view_source_item_count();
    if (!library_search_filter_active()) {
        g_search.match_count = source_count;
        quick_index_keyboard_set_selected(&g_search_keyboard, -1);
        return;
    }

    if (!library_search_ensure_capacity(source_count)) {
        g_search.match_count = 0U;
        quick_index_keyboard_set_selected(&g_search_keyboard, -1);
        return;
    }

    uint32_t count = 0U;
    for (uint32_t source_index = 0U; source_index < source_count; ++source_index) {
        if (library_search_source_matches(source_index)) {
            g_search.matches[count++] = source_index;
        }
    }
    g_search.match_count = count;
    quick_index_keyboard_set_selected(
        &g_search_keyboard,
        g_state.mode == LibraryBrowseMode::Decades ? static_cast<int8_t>(g_search.key_index) : -1);

    ESP_LOGI(TAG,
        "多首字母过滤：scope=%s query_len=%u decade_key=%u result=%lu/%lu",
        library_search_scope_name(),
        static_cast<unsigned>(g_search.query_length),
        static_cast<unsigned>(g_search.key_index),
        static_cast<unsigned long>(g_search.match_count),
        static_cast<unsigned long>(source_count));
}

static uint32_t library_view_source_item_index(uint32_t display_index)
{
    if (!g_search.active || !library_search_filter_active()) {
        return display_index;
    }
    if (display_index >= g_search.match_count || g_search.matches == nullptr) {
        return UINT32_MAX;
    }
    return g_search.matches[display_index];
}

static uint32_t library_view_item_count()
{
    return g_search.active ? g_search.match_count : library_view_source_item_count();
}

static int32_t library_view_max_scroll_y()
{
    const uint32_t item_count = library_view_item_count();
    const int32_t list_h = library_view_list_h();
    const int32_t content_h = item_count == 0U
        ? list_h
        : static_cast<int32_t>(item_count) * library_view_row_step() + 4;
    const int32_t max_scroll = content_h - list_h;
    return max_scroll > 0 ? max_scroll : 0;
}

static int32_t library_view_clamp_scroll_y(int32_t y)
{
    if (y < 0) {
        return 0;
    }
    const int32_t max_scroll = library_view_max_scroll_y();
    return y > max_scroll ? max_scroll : y;
}

static int32_t library_view_current_scroll_y()
{
    return g_manual_scroll_y;
}

static void library_view_set_scroll_y(int32_t y)
{
    const int32_t clamped = library_view_clamp_scroll_y(y);
    if (clamped == g_manual_scroll_y) {
        return;
    }
    g_manual_scroll_y = clamped;
    library_view_refresh_virtual_rows(false);
    library_view_scrollbar_update_position();
}

static void library_view_scrollbar_set_visible(bool visible)
{
    if (g_scrollbar.track == nullptr || g_scrollbar.thumb == nullptr) {
        return;
    }
    if (g_scrollbar.visible == visible) {
        return;
    }
    g_scrollbar.visible = visible;
    if (visible) {
        lv_obj_remove_flag(g_scrollbar.track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_scrollbar.thumb, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_scrollbar.track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_scrollbar.thumb, LV_OBJ_FLAG_HIDDEN);
        g_scrollbar.last_touch_update_tick_ms = 0U;
        g_scrollbar.thumb_y_valid = false;
    }
}

static void library_view_scrollbar_update_position()
{
    if (!g_scrollbar.visible || g_scrollbar.thumb == nullptr ||
        g_scrollbar.max_scroll <= 0 || g_scrollbar.travel < 0) {
        return;
    }

    // 列表本体必须逐触摸采样跟手；右侧位置条只是辅助视觉。
    // 手指按住期间把 thumb 更新限到约20Hz，释放后立即补一次最终位置。
    if (g_gesture.pressed) {
        const uint32_t now = static_cast<uint32_t>(lv_tick_get());
        if (g_scrollbar.last_touch_update_tick_ms != 0U &&
            static_cast<uint32_t>(now - g_scrollbar.last_touch_update_tick_ms) <
                LIBRARY_SCROLLBAR_TOUCH_UPDATE_MS) {
            return;
        }
        g_scrollbar.last_touch_update_tick_ms = now;
    } else {
        g_scrollbar.last_touch_update_tick_ms = 0U;
    }

    const int32_t thumb_offset = static_cast<int32_t>(
        (static_cast<int64_t>(g_scrollbar.travel) * static_cast<int64_t>(g_manual_scroll_y)) /
        static_cast<int64_t>(g_scrollbar.max_scroll));
    const int32_t thumb_y = g_scrollbar.track_y + thumb_offset;
    if (g_scrollbar.thumb_y_valid && g_scrollbar.last_thumb_y == thumb_y) {
        return;
    }
    lv_obj_set_y(g_scrollbar.thumb, thumb_y);
    g_scrollbar.last_thumb_y = thumb_y;
    g_scrollbar.thumb_y_valid = true;
}

static void library_view_scrollbar_rebuild_geometry()
{
    if (g_scrollbar.track == nullptr || g_scrollbar.thumb == nullptr) {
        return;
    }

    const int32_t list_y = library_view_list_y();
    const int32_t list_h = library_view_list_h();
    const int32_t track_h = list_h - LIBRARY_SCROLLBAR_MARGIN_Y * 2;
    const int32_t max_scroll = library_view_max_scroll_y();
    const uint32_t item_count = library_view_item_count();
    const int32_t content_h = item_count == 0U
        ? list_h
        : static_cast<int32_t>(item_count) * library_view_row_step() + 4;

    if (track_h <= 0 || max_scroll <= 0 || content_h <= list_h) {
        g_scrollbar.track_y = 0;
        g_scrollbar.track_h = 0;
        g_scrollbar.thumb_h = 0;
        g_scrollbar.travel = 0;
        g_scrollbar.max_scroll = 0;
        library_view_scrollbar_set_visible(false);
        return;
    }

    int32_t thumb_h = static_cast<int32_t>(
        (static_cast<int64_t>(track_h) * static_cast<int64_t>(list_h)) /
        static_cast<int64_t>(content_h));
    thumb_h = library_clamp_i32(thumb_h, LIBRARY_SCROLLBAR_MIN_THUMB_H, track_h);
    const int32_t x = FAKEPOD_LCD_WIDTH - LIBRARY_SCROLLBAR_MARGIN_RIGHT - LIBRARY_SCROLLBAR_W;
    const int32_t y = list_y + LIBRARY_SCROLLBAR_MARGIN_Y;

    g_scrollbar.track_y = y;
    g_scrollbar.track_h = track_h;
    g_scrollbar.thumb_h = thumb_h;
    g_scrollbar.travel = track_h - thumb_h;
    g_scrollbar.max_scroll = max_scroll;
    g_scrollbar.last_touch_update_tick_ms = 0U;
    g_scrollbar.thumb_y_valid = false;

    lv_obj_set_pos(g_scrollbar.track, x, y);
    lv_obj_set_size(g_scrollbar.track, LIBRARY_SCROLLBAR_W, track_h);
    lv_obj_set_x(g_scrollbar.thumb, x);
    lv_obj_set_size(g_scrollbar.thumb, LIBRARY_SCROLLBAR_W, thumb_h);
    library_view_scrollbar_set_visible(true);
    library_view_scrollbar_update_position();
}

static bool library_view_inertia_audio_safe()
{
    FlacStorageWindowSnapshot window = {};
    if (!flac_decoder_get_storage_window(&window) || !window.active) {
        return true;
    }

    // P1.3.5.4.2：任何 FLAC 播放期间都关闭惯性。
    // Direct Touch 仍然逐采样跟手；只取消抬手后的 16ms 动画，给 FlacPrefetch 留足 Core1 余量。
    return false;
}

static void library_view_store_scroll_position()
{
    if (g_list_host == nullptr) {
        return;
    }
    const int32_t y = library_view_current_scroll_y();
    if (g_search.active) {
        g_search.scroll_y = y;
        return;
    }
    if (library_view_folder_browser_active()) {
        const uint8_t view = static_cast<uint8_t>(g_state.folder_view);
        if (view >= 1U && view <= 3U) {
            g_state.folder_scroll_y[view - 1U] = y;
        }
        return;
    }
    if (g_state.mode == LibraryBrowseMode::GroupTracks) {
        g_state.detail_scroll_y = y;
        return;
    }
    const uint8_t mode = static_cast<uint8_t>(g_state.mode);
    if (mode < 4U) {
        g_state.top_scroll_y[mode] = y;
    }
}

static void library_view_inertia_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!g_inertia.active || !library_view_is_visible() || g_gesture.pressed) {
        library_view_inertia_stop(true);
        return;
    }
    if (!library_view_inertia_audio_safe()) {
        library_view_inertia_stop(true);
        return;
    }

    const uint32_t now = static_cast<uint32_t>(lv_tick_get());
    uint32_t dt_ms = now - g_inertia.last_tick_ms;
    if (dt_ms == 0U) {
        return;
    }
    if (dt_ms > LIBRARY_INERTIA_DT_MAX_MS) {
        dt_ms = LIBRARY_INERTIA_DT_MAX_MS;
    }
    g_inertia.last_tick_ms = now;

    const int64_t accumulated =
        static_cast<int64_t>(g_inertia.velocity_px_s) * static_cast<int64_t>(dt_ms) +
        static_cast<int64_t>(g_inertia.remainder_milli_px);
    const int32_t delta_px = static_cast<int32_t>(accumulated / 1000LL);
    g_inertia.remainder_milli_px = static_cast<int32_t>(accumulated % 1000LL);

    if (delta_px != 0) {
        const int32_t before = library_view_current_scroll_y();
        library_view_set_scroll_y(before + delta_px);
        const int32_t after = library_view_current_scroll_y();
        if (after == before) {
            library_view_inertia_stop(true);
            return;
        }
        g_gesture.suppress_click_until = now + LIBRARY_SUPPRESS_CLICK_MS;
    }

    g_inertia.velocity_px_s =
        (g_inertia.velocity_px_s * LIBRARY_INERTIA_DECAY_PERCENT) / 100;
    if (library_abs(g_inertia.velocity_px_s) < LIBRARY_INERTIA_STOP_PX_S) {
        library_view_inertia_stop(true);
    }
}

static void library_view_inertia_stop(bool store_position)
{
    const bool was_active = g_inertia.active;
    g_inertia.active = false;
    g_inertia.velocity_px_s = 0;
    g_inertia.remainder_milli_px = 0;
    g_inertia.last_tick_ms = 0U;
    if (g_inertia.timer != nullptr) {
        lv_timer_pause(g_inertia.timer);
    }
    if (was_active && store_position) {
        library_view_store_scroll_position();
    }
}

static void library_view_inertia_start(int32_t velocity_px_s, uint32_t tick_ms)
{
    if (g_inertia.timer == nullptr || !library_view_inertia_audio_safe()) {
        return;
    }
    const int32_t velocity = library_clamp_i32(
        velocity_px_s, -LIBRARY_INERTIA_MAX_PX_S, LIBRARY_INERTIA_MAX_PX_S);
    if (library_abs(velocity) < LIBRARY_INERTIA_START_MIN_PX_S) {
        return;
    }

    g_inertia.velocity_px_s = velocity;
    g_inertia.remainder_milli_px = 0;
    g_inertia.last_tick_ms = tick_ms;
    g_inertia.active = true;
    g_gesture.suppress_click_until = tick_ms + LIBRARY_SUPPRESS_CLICK_MS;
    lv_timer_reset(g_inertia.timer);
    lv_timer_resume(g_inertia.timer);
}

static int32_t library_view_saved_scroll_position()
{
    if (g_search.active) {
        return g_search.scroll_y;
    }
    if (library_view_folder_browser_active()) {
        const uint8_t view = static_cast<uint8_t>(g_state.folder_view);
        return view >= 1U && view <= 3U ? g_state.folder_scroll_y[view - 1U] : 0;
    }
    if (g_state.mode == LibraryBrowseMode::GroupTracks) {
        return g_state.detail_scroll_y;
    }
    const uint8_t mode = static_cast<uint8_t>(g_state.mode);
    return mode < 4U ? g_state.top_scroll_y[mode] : 0;
}

static int32_t library_view_scroll_for_position(uint32_t position)
{
    // 当前曲目尽量落在五行中间，首尾自动由 LVGL clamp。
    const uint32_t top_position = position > 2U ? position - 2U : 0U;
    return static_cast<int32_t>(top_position * static_cast<uint32_t>(LIBRARY_ROW_STEP));
}

static const char *library_search_key_label(uint8_t key_index)
{
    static const char *const alpha_labels[QUICK_INDEX_KEY_COUNT] = {
        "ABC", "DEF", "GHI", "JKL", "MNO",
        "PQRS", "TUV", "WXYZ", "0-9/#", "全部",
    };
    static const char *const decade_labels[QUICK_INDEX_KEY_COUNT] = {
        "1950", "1960", "1970", "1980", "1990",
        "2000", "2010", "2020", "未知", "全部",
    };
    const char *const *labels = g_state.mode == LibraryBrowseMode::Decades ? decade_labels : alpha_labels;
    return key_index < QUICK_INDEX_KEY_COUNT ? labels[key_index] : "全部";
}

static const char *library_search_scope_name()
{
    if (g_state.mode == LibraryBrowseMode::Artists) return "歌手";
    if (g_state.mode == LibraryBrowseMode::Albums) return "专辑";
    if (g_state.mode == LibraryBrowseMode::Decades) return "年代";
    return "歌曲";
}

static void library_search_format_query(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    size_t used = 0U;
    for (uint8_t i = 0U; i < g_search.query_length; ++i) {
        const char *label = library_search_key_label(g_search.query[i]);
        const int written = snprintf(
            out + used,
            out_size - used,
            "%s%s",
            i == 0U ? "" : "·",
            label);
        if (written <= 0) {
            break;
        }
        const size_t added = static_cast<size_t>(written);
        if (added >= out_size - used) {
            used = out_size - 1U;
            break;
        }
        used += added;
    }
}

static void library_view_apply_scope_icon()
{
    if (g_scope_icon == nullptr || g_scope_icon_front == nullptr ||
        g_scope_icon_front_tab == nullptr || g_scope_icon_back == nullptr ||
        g_scope_icon_back_tab == nullptr) {
        return;
    }

    const PlayerFolderScope scope = player_control_get_folder_scope();
    if (scope == PlayerFolderScope::All) {
        lv_obj_add_flag(g_scope_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const LibraryThemeColors theme = library_view_theme_colors();
    const lv_color_t color = lv_color_hex(theme.accent);
    lv_obj_remove_flag(g_scope_icon, LV_OBJ_FLAG_HIDDEN);

    // 前层文件夹：一级/二级共用。图标全部由 LVGL 基础对象绘制，不依赖 emoji/字体资源。
    lv_obj_set_style_border_color(g_scope_icon_front, color, 0);
    lv_obj_set_style_bg_color(g_scope_icon_front, color, 0);
    lv_obj_set_style_border_color(g_scope_icon_front_tab, color, 0);
    lv_obj_set_style_bg_color(g_scope_icon_front_tab, color, 0);

    const bool level2 = scope == PlayerFolderScope::Level2;
    if (level2) {
        lv_obj_set_style_border_color(g_scope_icon_back, color, 0);
        lv_obj_set_style_bg_color(g_scope_icon_back, color, 0);
        lv_obj_set_style_border_color(g_scope_icon_back_tab, color, 0);
        lv_obj_set_style_bg_color(g_scope_icon_back_tab, color, 0);
        lv_obj_remove_flag(g_scope_icon_back, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_scope_icon_back_tab, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_scope_icon_back, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_scope_icon_back_tab, LV_OBJ_FLAG_HIDDEN);
    }
}

static void library_view_set_header()
{
    if (g_header_title == nullptr) {
        return;
    }

    if (g_search.active) {
        static char search_title[96] = {};
        if (g_state.mode == LibraryBrowseMode::Decades) {
            if (g_search.key_index == static_cast<uint8_t>(LibrarySearchBucket::All)) {
                snprintf(search_title, sizeof(search_title), "搜索%s", library_search_scope_name());
            } else {
                snprintf(search_title, sizeof(search_title), "%s · %s",
                    library_search_scope_name(), library_search_key_label(g_search.key_index));
            }
        } else if (g_search.query_length == 0U) {
            snprintf(search_title, sizeof(search_title), "搜索%s", library_search_scope_name());
        } else {
            char query_text[64] = {};
            library_search_format_query(query_text, sizeof(query_text));
            snprintf(search_title, sizeof(search_title), "%s · %s", library_search_scope_name(), query_text);
        }
        lv_label_set_text(g_header_title, search_title);
    } else if (library_view_folder_browser_active()) {
        if (g_state.folder_view == LibraryFolderView::Level1Folders) {
            lv_label_set_text(g_header_title, "Music");
        } else if (g_state.folder_view == LibraryFolderView::Level2Parents) {
            lv_label_set_text(g_header_title, "Music");
        } else if (g_state.folder_view == LibraryFolderView::Level2Folders) {
            char parent_copy[PLAYER_FOLDER_PATH_MAX] = {};
            if (library_view_resolve_level2_parent()) {
                snprintf(parent_copy, sizeof(parent_copy), "%s", g_state.level2_parent_path);
            }
            const char *parent_name = library_folder_leaf_in_place(parent_copy);
            lv_label_set_text(g_header_title, parent_name != nullptr ? parent_name : "文件夹");
        }
    } else if (library_view_folder_scope_active()) {
        char folder_label[96] = {};
        if (player_state_copy_list_label(folder_label, sizeof(folder_label))) {
            lv_label_set_text(g_header_title, folder_label);
        } else {
            lv_label_set_text(
                g_header_title,
                player_playlist_folder_scope_name(player_control_get_folder_scope()));
        }
    } else if (g_state.mode == LibraryBrowseMode::GroupTracks) {
        const char *title = nullptr;
        if (library_view_get_detail_identity(&title, nullptr, nullptr) && title != nullptr && title[0] != '\0') {
            lv_label_set_text(g_header_title, title);
        } else {
            lv_label_set_text(g_header_title, "曲库详情");
        }
    } else {
        switch (g_state.mode) {
            case LibraryBrowseMode::AllTracks: lv_label_set_text(g_header_title, "歌曲"); break;
            case LibraryBrowseMode::Artists: lv_label_set_text(g_header_title, "歌手"); break;
            case LibraryBrowseMode::Albums: lv_label_set_text(g_header_title, "专辑"); break;
            case LibraryBrowseMode::Decades: lv_label_set_text(g_header_title, "年代"); break;
            case LibraryBrowseMode::GroupTracks: break;
        }
    }

    const LibraryThemeColors theme = library_view_theme_colors();
    if (g_root != nullptr) {
        lv_obj_set_style_bg_color(g_root, lv_color_hex(theme.page_bg), 0);
    }
    lv_obj_set_style_text_color(g_header_title, lv_color_hex(theme.header), 0);

    if (g_back_button != nullptr) {
        lv_obj_remove_flag(g_back_button, LV_OBJ_FLAG_HIDDEN);
    }

    library_view_apply_scope_icon();
    const bool folder_scope = player_control_get_folder_scope() != PlayerFolderScope::All;
    lv_obj_set_x(g_header_title, folder_scope ? LIBRARY_SCOPE_TITLE_X : LIBRARY_TITLE_X);
    lv_obj_set_width(g_header_title, folder_scope ? LIBRARY_SCOPE_TITLE_W : LIBRARY_TITLE_W);
    lv_obj_set_style_text_align(g_header_title, LV_TEXT_ALIGN_CENTER, 0);
}

static void library_view_apply_indicator()
{
    const LibraryBrowseMode active = library_view_category_mode_for_detail();
    for (uint32_t i = 0; i < 4U; ++i) {
        if (g_indicator[i] == nullptr) {
            continue;
        }
        if (g_search.active || library_view_folder_scope_active()) {
            lv_obj_add_flag(g_indicator[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(g_indicator[i], LV_OBJ_FLAG_HIDDEN);
        const bool selected = static_cast<uint8_t>(active) == i;
        const LibraryThemeColors theme = library_view_theme_colors();
        lv_obj_set_style_bg_color(
            g_indicator[i],
            lv_color_hex(selected ? theme.accent : 0x343B47),
            0);
        lv_obj_set_style_bg_opa(g_indicator[i], selected ? LV_OPA_COVER : LV_OPA_60, 0);
    }
}

static void library_search_apply_keyboard_layout()
{
    static const char *const alpha_labels_all[QUICK_INDEX_KEY_COUNT] = {
        "ABC", "DEF", "GHI", "JKL", "MNO",
        "PQRS", "TUV", "WXYZ", "0-9/#", "全部",
    };
    static const char *const alpha_labels_back[QUICK_INDEX_KEY_COUNT] = {
        "ABC", "DEF", "GHI", "JKL", "MNO",
        "PQRS", "TUV", "WXYZ", "0-9/#", "退格",
    };
    static const char *const decade_labels[QUICK_INDEX_KEY_COUNT] = {
        "1950", "1960", "1970", "1980", "1990",
        "2000", "2010", "2020", "未知", "全部",
    };

    if (g_state.mode == LibraryBrowseMode::Decades) {
        quick_index_keyboard_set_labels(&g_search_keyboard, decade_labels);
        quick_index_keyboard_set_selected(&g_search_keyboard, static_cast<int8_t>(g_search.key_index));
        return;
    }

    quick_index_keyboard_set_labels(
        &g_search_keyboard,
        g_search.query_length > 0U ? alpha_labels_back : alpha_labels_all);
    quick_index_keyboard_set_selected(&g_search_keyboard, -1);
}

static void library_view_apply_list_layout()
{
    if (g_list_host == nullptr) {
        return;
    }

    const int32_t row_h = library_view_row_h();
    lv_obj_set_pos(g_list_host, LIBRARY_LIST_X, library_view_list_y());
    lv_obj_set_size(g_list_host, LIBRARY_LIST_W, library_view_list_h());

    for (uint32_t slot = 0U; slot < LIBRARY_VIRTUAL_ROWS; ++slot) {
        LibraryVirtualRow &row = g_rows[slot];
        if (row.button == nullptr) {
            continue;
        }
        lv_obj_set_size(row.button, LIBRARY_ROW_W, row_h);
        if (row.accent != nullptr) {
            const int32_t accent_y = g_search.active ? 8 : 12;
            const int32_t accent_h = g_search.active ? 28 : 38;
            lv_obj_set_pos(row.accent, 0, accent_y);
            lv_obj_set_height(row.accent, accent_h);
        }
        if (row.label != nullptr) {
            lv_obj_set_pos(row.label, 18, g_search.active ? 6 : 15);
            lv_obj_set_height(row.label, g_search.active ? 30 : 32);
        }
        if (row.arrow != nullptr) {
            lv_obj_align(row.arrow, LV_ALIGN_RIGHT_MID, -16, 0);
        }
    }

    quick_index_keyboard_set_visible(&g_search_keyboard, g_search.active);
    if (g_search_button != nullptr) {
        if (library_view_folder_browser_active()) {
            lv_obj_add_flag(g_search_button, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(g_search_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(
                g_search_button,
                lv_color_hex(g_search.active ? 0x2D3745 : 0x171C24),
                LV_STATE_DEFAULT);
        }
    }
    if (g_search.active) {
        library_search_apply_keyboard_layout();
    }
    library_view_scrollbar_rebuild_geometry();
}

static void library_search_enter()
{
    if (g_search.active || library_view_folder_browser_active()) {
        return;
    }

    library_view_store_scroll_position();
    g_search.active = true;
    g_search.parent_scroll_y = g_manual_scroll_y;
    g_search.scroll_y = 0;
    g_search.key_index = static_cast<uint8_t>(LibrarySearchBucket::All);
    g_search.query_length = 0U;
    if (g_state.mode != LibraryBrowseMode::Decades) {
        library_search_build_key_cache();
    }
    library_search_rebuild_matches();
    library_view_render(false);
    UI_PAGE_INTERACTION_LOGI("进入快速搜索：scope=%s source=%lu",
        library_search_scope_name(),
        static_cast<unsigned long>(library_view_source_item_count()));
}

static void library_search_exit()
{
    if (!g_search.active) {
        return;
    }

    const int32_t restore_scroll = g_search.parent_scroll_y;
    g_search.active = false;
    g_search.key_index = static_cast<uint8_t>(LibrarySearchBucket::All);
    g_search.query_length = 0U;
    g_search.match_count = 0U;
    g_search.scroll_y = 0;
    quick_index_keyboard_set_visible(&g_search_keyboard, false);
    g_manual_scroll_y = library_view_clamp_scroll_y(restore_scroll);
    library_view_render(true);
    UI_PAGE_INTERACTION_LOGI("退出快速搜索：恢复scroll=%ld", static_cast<long>(restore_scroll));
}

static void library_search_keyboard_cb(uint8_t key_index, void *user_data)
{
    (void)user_data;
    if (!g_search.active || key_index >= QUICK_INDEX_KEY_COUNT) {
        return;
    }

    if (g_state.mode == LibraryBrowseMode::Decades) {
        g_search.key_index = key_index;
        library_search_rebuild_matches();
        library_view_render(false);
        return;
    }

    if (key_index == static_cast<uint8_t>(LibrarySearchBucket::All)) {
        if (g_search.query_length > 0U) {
            --g_search.query_length;
        }
    } else if (key_index <= static_cast<uint8_t>(LibrarySearchBucket::Other)) {
        if (g_search.query_length >= LIBRARY_SEARCH_QUERY_MAX) {
            ESP_LOGW(TAG, "多首字母查询已达上限：%u位", static_cast<unsigned>(LIBRARY_SEARCH_QUERY_MAX));
            return;
        }
        g_search.query[g_search.query_length++] = key_index;
    }

    library_search_rebuild_matches();
    library_view_render(false);
}

static void library_view_set_row_text(LibraryVirtualRow &row, const char *primary, const char *secondary)
{
    const char *safe_primary = primary != nullptr && primary[0] != '\0' ? primary : "未知";
    lv_obj_set_y(row.label, 15);
    lv_obj_set_height(row.label, 32);
    if (row.meta != nullptr) {
        lv_obj_add_flag(row.meta, LV_OBJ_FLAG_HIDDEN);
    }
    if (secondary != nullptr && secondary[0] != '\0') {
        lv_label_set_text_fmt(row.label, "%s · %s", safe_primary, secondary);
    } else {
        lv_label_set_text(row.label, safe_primary);
    }
}

static void library_view_set_folder_row_text(
    LibraryVirtualRow &row, const char *name, uint32_t count)
{
    const char *safe_name = name != nullptr && name[0] != '\0' ? name : "未知";
    lv_label_set_text(row.label, safe_name);
    lv_obj_set_y(row.label, 15);
    lv_obj_set_height(row.label, 32);
    lv_obj_set_width(row.label, LIBRARY_ROW_W - 146);
    if (row.meta != nullptr) {
        // 文件夹行保持纯粹的左右结构：左侧名称，右侧仅数量数字。
        // 当前目录由左侧 accent 色标表达，不再重复“当前”文字。
        lv_label_set_text_fmt(row.meta, "%lu", static_cast<unsigned long>(count));
        lv_obj_set_y(row.meta, 15);
        lv_obj_set_height(row.meta, 32);
        lv_obj_remove_flag(row.meta, LV_OBJ_FLAG_HIDDEN);
    }
}

static void library_view_reset_row_style(LibraryVirtualRow &row, bool current, bool arrow)
{
    const LibraryThemeColors theme = library_view_theme_colors();
    lv_obj_set_style_bg_color(row.button, lv_color_hex(current ? theme.row_current_bg : theme.row_bg), 0);
    lv_obj_set_style_text_color(row.label, lv_color_hex(current ? 0xFFFFFF : 0xE9ECF1), 0);
    if (row.accent != nullptr) {
        lv_obj_set_style_bg_color(row.accent, lv_color_hex(theme.accent), 0);
        if (current && library_view_folder_browser_active()) {
            // 文件夹选择页的当前项用一个小圆点表达，不占用“当前”文字空间。
            lv_obj_set_pos(row.accent, 7, 27);
            lv_obj_set_size(row.accent, 8, 8);
            lv_obj_set_style_radius(row.accent, 4, 0);
        } else {
            // 歌曲当前项继续沿用原来的细竖条。
            lv_obj_set_pos(row.accent, 0, 12);
            lv_obj_set_size(row.accent, 3, 38);
            lv_obj_set_style_radius(row.accent, 2, 0);
        }
        if (current) lv_obj_remove_flag(row.accent, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(row.accent, LV_OBJ_FLAG_HIDDEN);
    }
    if (row.arrow != nullptr) {
        lv_obj_set_style_text_color(row.arrow, lv_color_hex(theme.accent), 0);
        if (arrow) lv_obj_remove_flag(row.arrow, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(row.arrow, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_width(row.label, arrow ? LIBRARY_ROW_W - 62 : LIBRARY_ROW_W - 36);
}

static bool library_view_bind_track_row(LibraryVirtualRow &row, uint32_t position, uint32_t track_index, bool detail_row)
{
    MediaTrackViewV2 track = {};
    if (!media_catalog_v2_get_track_view(track_index, &track) || track.row == nullptr) {
        return false;
    }

    const bool current = player_state_is_ready() && player_state_get_index() == track_index;
    const char *secondary = nullptr;
    if (track.artist != nullptr && track.artist[0] != '\0') {
        secondary = track.artist;
    } else if (track.album != nullptr && track.album[0] != '\0') {
        secondary = track.album;
    } else {
        secondary = media_format_name(track.row->format);
    }

    library_view_set_row_text(row, track.title, secondary);
    library_view_reset_row_style(row, current, false);

    row.binding.generation = media_catalog_v2_generation();
    row.binding.group_index = detail_row ? g_state.detail_group_index : UINT32_MAX;
    row.binding.position = position;
    if (detail_row) {
        row.binding.action = LibraryRowAction::PlayGroupTrack;
        row.binding.group_type = g_state.detail_type;
    } else {
        row.binding.action = library_view_folder_scope_active()
            ? LibraryRowAction::PlayFolderTrack
            : LibraryRowAction::PlayAllTrack;
        row.binding.group_type = PlayerListType::AllTracks;
    }
    return true;
}

static bool library_view_bind_folder_row(LibraryVirtualRow &row, uint32_t item_index)
{
    row.binding = {};
    row.binding.generation = media_catalog_v2_generation();

    if (g_state.folder_view == LibraryFolderView::Level2Folders && item_index == 0U) {
        library_view_set_row_text(row, "上一级", nullptr);
        library_view_reset_row_style(row, false, true);
        row.binding.action = LibraryRowAction::FolderUp;
        return true;
    }

    PlayerFolderScope option_scope = PlayerFolderScope::Level1;
    const char *parent_path = nullptr;
    size_t option_position = item_index;
    uint32_t child_count = 0U;
    LibraryRowAction action = LibraryRowAction::SelectLevel1Folder;
    if (g_state.folder_view == LibraryFolderView::Level2Folders) {
        if (!library_view_resolve_level2_parent() || item_index == 0U) return false;
        option_scope = PlayerFolderScope::Level2;
        parent_path = g_state.level2_parent_path;
        option_position = static_cast<size_t>(item_index - 1U);
        action = LibraryRowAction::SelectLevel2Folder;
    } else if (g_state.folder_view == LibraryFolderView::Level2Parents) {
        action = LibraryRowAction::SelectLevel2Parent;
    }

    char path[PLAYER_FOLDER_PATH_MAX] = {};
    uint32_t track_count = 0U;
    if (g_state.folder_view == LibraryFolderView::Level2Parents) {
        if (!library_view_level2_parent_at(
                item_index, &option_position, path, sizeof(path), &child_count)) {
            return false;
        }
    } else if (!player_playlist_copy_folder_option_at(
            option_scope,
            parent_path,
            option_position,
            path,
            sizeof(path),
            &track_count)) {
        return false;
    }

    char name_path[PLAYER_FOLDER_PATH_MAX] = {};
    snprintf(name_path, sizeof(name_path), "%s", path);
    const char *name = library_folder_leaf_in_place(name_path);
    if (name == nullptr) return false;

    bool current = false;
    char current_path[PLAYER_FOLDER_PATH_MAX] = {};
    if (g_state.folder_view == LibraryFolderView::Level1Folders) {
        current = player_control_copy_folder_selection(
            PlayerFolderScope::Level1, current_path, sizeof(current_path)) &&
            strcasecmp(current_path, path) == 0;
    } else if (g_state.folder_view == LibraryFolderView::Level2Folders) {
        current = player_control_copy_folder_selection(
            PlayerFolderScope::Level2, current_path, sizeof(current_path)) &&
            strcasecmp(current_path, path) == 0;
    } else if (g_state.folder_view == LibraryFolderView::Level2Parents) {
        current = g_state.level2_parent_path[0] != '\0' &&
            strcasecmp(g_state.level2_parent_path, path) == 0;
    }

    const uint32_t folder_count = g_state.folder_view == LibraryFolderView::Level2Parents
        ? child_count
        : track_count;
    library_view_set_folder_row_text(row, name, folder_count);
    library_view_reset_row_style(row, current, true);
    // reset_row_style() 会按箭头恢复通用宽度；文件夹行再次收窄，给右侧数量留固定空间。
    lv_obj_set_width(row.label, LIBRARY_ROW_W - 146);
    row.binding.action = action;
    row.binding.position = static_cast<uint32_t>(option_position);
    return true;
}

static bool library_view_bind_top_row(LibraryVirtualRow &row, uint32_t item_index)
{
    row.binding = {};
    row.binding.generation = media_catalog_v2_generation();
    row.binding.group_index = item_index;

    if (g_state.mode == LibraryBrowseMode::AllTracks) {
        uint32_t track_index = UINT32_MAX;
        return library_view_top_track_index(item_index, &track_index) &&
            library_view_bind_track_row(row, item_index, track_index, false);
    }

    char secondary[96] = {};
    const char *primary = "未知";

    if (g_state.mode == LibraryBrowseMode::Artists) {
        MediaArtistGroupViewV2 view = {};
        if (!media_groups_v2_get_artist(item_index, &view)) {
            return false;
        }
        row.binding.generation = view.generation;
        row.binding.action = LibraryRowAction::OpenArtist;
        row.binding.group_type = PlayerListType::Artist;
        primary = view.name;
        snprintf(secondary, sizeof(secondary), "%lu首", static_cast<unsigned long>(view.track_count));
    } else if (g_state.mode == LibraryBrowseMode::Albums) {
        MediaAlbumGroupViewV2 view = {};
        if (!media_groups_v2_get_album(item_index, &view)) {
            return false;
        }
        row.binding.generation = view.generation;
        row.binding.action = LibraryRowAction::OpenAlbum;
        row.binding.group_type = PlayerListType::Album;
        primary = view.title;
        if (view.artist != nullptr && view.artist[0] != '\0') {
            library_view_set_row_text(row, primary, view.artist);
            library_view_reset_row_style(row, false, true);
            return true;
        }
        snprintf(secondary, sizeof(secondary), "%lu首", static_cast<unsigned long>(view.track_count));
    } else if (g_state.mode == LibraryBrowseMode::Decades) {
        MediaDecadeGroupViewV2 view = {};
        if (!media_groups_v2_get_decade(item_index, &view)) {
            return false;
        }
        row.binding.generation = view.generation;
        row.binding.action = LibraryRowAction::OpenDecade;
        row.binding.group_type = PlayerListType::Decade;
        static char decade_labels[LIBRARY_VIRTUAL_ROWS][32] = {};
        const uint32_t slot = static_cast<uint32_t>(&row - &g_rows[0]);
        if (slot >= LIBRARY_VIRTUAL_ROWS) {
            return false;
        }
        if (view.unknown) {
            snprintf(decade_labels[slot], sizeof(decade_labels[slot]), "未知年代");
        } else {
            snprintf(decade_labels[slot], sizeof(decade_labels[slot]), "%u年代", static_cast<unsigned>(view.decade_start));
        }
        primary = decade_labels[slot];
        snprintf(secondary, sizeof(secondary), "%lu首", static_cast<unsigned long>(view.track_count));
    }

    library_view_set_row_text(row, primary, secondary);
    library_view_reset_row_style(row, false, true);
    return true;
}

static bool library_view_bind_detail_row(LibraryVirtualRow &row, uint32_t position)
{
    uint32_t track_index = UINT32_MAX;
    uint32_t generation = 0U;
    if (!library_view_detail_track_index(position, &track_index, &generation)) {
        return false;
    }
    if (!library_view_bind_track_row(row, position, track_index, true)) {
        return false;
    }
    row.binding.generation = generation;
    return true;
}

static void library_view_hide_row(LibraryVirtualRow &row)
{
    row.item_index = UINT32_MAX;
    row.binding = {};
    if (row.button != nullptr) {
        lv_obj_add_flag(row.button, LV_OBJ_FLAG_HIDDEN);
    }
}

static void library_view_refresh_virtual_rows(bool force)
{
    if (g_list_host == nullptr) {
        return;
    }

    const uint32_t item_count = library_view_item_count();
    const int32_t row_step = library_view_row_step();
    int32_t scroll_y = library_view_current_scroll_y();
    uint32_t first_visible = scroll_y > 0 ? static_cast<uint32_t>(scroll_y / row_step) : 0U;
    uint32_t first_item = first_visible > 0U ? first_visible - 1U : 0U;
    for (uint32_t slot = 0U; slot < LIBRARY_VIRTUAL_ROWS; ++slot) {
        LibraryVirtualRow &row = g_rows[slot];
        const uint32_t item_index = first_item + slot;
        if (item_index >= item_count || row.button == nullptr) {
            library_view_hide_row(row);
            continue;
        }

        // 直驱滚动时每个 CST820 采样都更新屏幕位置；只有跨到新的显示 item 时才重绑文本。
        // 搜索模式下 display index 再映射回真实歌曲/分组索引，不复制任何字符串。
        lv_obj_set_y(row.button, static_cast<int32_t>(item_index) * row_step + 4 - scroll_y);
        if (!force && row.item_index == item_index) {
            continue;
        }

        const uint32_t source_index = library_view_source_item_index(item_index);
        if (source_index == UINT32_MAX) {
            library_view_hide_row(row);
            continue;
        }

        row.binding = {};
        row.item_index = item_index;
        const bool ok = library_view_folder_browser_active()
            ? library_view_bind_folder_row(row, source_index)
            : (g_state.mode == LibraryBrowseMode::GroupTracks
                ? library_view_bind_detail_row(row, source_index)
                : library_view_bind_top_row(row, source_index));
        if (ok) {
            lv_obj_remove_flag(row.button, LV_OBJ_FLAG_HIDDEN);
        } else {
            library_view_hide_row(row);
        }
    }
}

static void library_view_show_empty_if_needed()
{
    if (g_hint == nullptr) {
        return;
    }
    if (library_view_item_count() == 0U) {
        lv_label_set_text(g_hint, g_search.active ? "没有匹配结果" : "暂无内容");
        lv_obj_remove_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void library_view_render(bool preserve_scroll)
{
    if (g_root == nullptr || g_list_host == nullptr) {
        return;
    }

    library_view_inertia_stop(false);
    const int32_t target_scroll = preserve_scroll ? library_view_saved_scroll_position() : 0;
    library_view_set_header();
    library_view_apply_indicator();
    library_view_apply_list_layout();
    g_manual_scroll_y = library_view_clamp_scroll_y(target_scroll);
    library_view_refresh_virtual_rows(true);
    library_view_show_empty_if_needed();
}

static void library_view_schedule_render_async(void *user_data)
{
    (void)user_data;
    library_view_render(true);
}

static void library_view_schedule_render()
{
    lv_async_call(library_view_schedule_render_async, nullptr);
}

static void library_view_close_to_home()
{
    if (g_root == nullptr) {
        return;
    }
    library_view_store_scroll_position();
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    player_home_resume_from_fullscreen_view("library-back");
    UI_PAGE_INTERACTION_LOGI("曲库返回主页");
}

static void library_view_switch_folder_panel(int direction)
{
    if (!library_view_folder_scope_active() || g_search.active || direction == 0) {
        return;
    }

    const PlayerFolderScope scope = player_control_get_folder_scope();
    LibraryFolderView next = g_state.folder_view;
    if (direction > 0) { // 左滑：歌曲 -> 文件夹。
        if (g_state.folder_view == LibraryFolderView::Tracks) {
            if (scope == PlayerFolderScope::Level1) {
                next = LibraryFolderView::Level1Folders;
            } else if (scope == PlayerFolderScope::Level2) {
                (void)library_view_resolve_level2_parent();
                next = LibraryFolderView::Level2Folders;
            }
        }
    } else { // 右滑：文件夹 -> 歌曲；二级父目录先退回二级文件夹。
        if (g_state.folder_view == LibraryFolderView::Level2Parents) {
            next = LibraryFolderView::Level2Folders;
        } else if (g_state.folder_view != LibraryFolderView::Tracks) {
            next = LibraryFolderView::Tracks;
        }
    }

    if (next == g_state.folder_view) return;
    library_view_store_scroll_position();
    g_state.folder_view = next;
    library_view_render(true);
    UI_PAGE_INTERACTION_LOGI("目录横滑切换：scope=%s view=%u",
        player_playlist_folder_scope_name(scope),
        static_cast<unsigned>(g_state.folder_view));
}

static void library_view_switch_category(int direction)
{
    if (g_search.active || library_view_folder_scope_active() ||
        g_state.mode == LibraryBrowseMode::GroupTracks || direction == 0) {
        return;
    }

    library_view_store_scroll_position();
    int32_t index = static_cast<int32_t>(static_cast<uint8_t>(g_state.mode));
    index += direction;
    if (index < 0 || index > 3) {
        return; // 方屏横滑按顺序切换，不在首尾循环跳转。
    }

    g_state.mode = static_cast<LibraryBrowseMode>(index);
    g_state.detail_type = PlayerListType::AllTracks;
    g_state.detail_group_index = UINT32_MAX;
    library_view_render(true);
    UI_PAGE_INTERACTION_LOGI("曲库横滑切换：mode=%ld", static_cast<long>(index));
}

static void library_view_apply_pending_gesture_async(void *user_data)
{
    (void)user_data;
    g_gesture.async_scheduled = false;
    const LibraryPendingGesture action = g_gesture.pending;
    g_gesture.pending = LibraryPendingGesture::None;

    switch (action) {
        case LibraryPendingGesture::SwipeLeft:
            if (library_view_folder_scope_active()) {
                library_view_switch_folder_panel(+1);
            } else {
                library_view_switch_category(+1);
            }
            break;
        case LibraryPendingGesture::SwipeRight:
            if (library_view_folder_scope_active()) {
                library_view_switch_folder_panel(-1);
            } else if (g_state.mode == LibraryBrowseMode::GroupTracks) {
                library_view_return_to_parent_category();
            } else {
                library_view_switch_category(-1);
            }
            break;
        case LibraryPendingGesture::None:
            break;
    }
}

static void library_view_queue_gesture(LibraryPendingGesture action, uint32_t tick_ms)
{
    if (action == LibraryPendingGesture::None) {
        return;
    }
    g_gesture.pending = action;
    g_gesture.suppress_click_until = tick_ms + LIBRARY_SUPPRESS_CLICK_MS;
    if (!g_gesture.async_scheduled) {
        g_gesture.async_scheduled = true;
        lv_async_call(library_view_apply_pending_gesture_async, nullptr);
    }
}

static void library_view_row_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || library_click_suppressed()) {
        return;
    }

    LibraryRowBinding *row = static_cast<LibraryRowBinding *>(lv_event_get_user_data(event));
    if (row == nullptr || row->generation == 0U || row->generation != media_catalog_v2_generation()) {
        ESP_LOGW(TAG, "点击行已过期，刷新曲库视图");
        library_view_schedule_render();
        return;
    }

    const bool selecting_track =
        row->action == LibraryRowAction::PlayAllTrack ||
        row->action == LibraryRowAction::PlayFolderTrack ||
        row->action == LibraryRowAction::PlayGroupTrack;
    const bool transition_hold = selecting_track && player_home_prepare_track_transition_hold();

    switch (row->action) {
        case LibraryRowAction::OpenArtist:
        case LibraryRowAction::OpenAlbum:
        case LibraryRowAction::OpenDecade:
        {
            const bool from_search = g_search.active;
            const int32_t parent_scroll = from_search
                ? g_search.parent_scroll_y
                : library_view_current_scroll_y();
            if (from_search) {
                g_search.active = false;
                g_search.key_index = static_cast<uint8_t>(LibrarySearchBucket::All);
                g_search.query_length = 0U;
                g_search.match_count = 0U;
                g_search.scroll_y = 0;
                quick_index_keyboard_set_visible(&g_search_keyboard, false);
            } else {
                library_view_store_scroll_position();
            }
            g_state.parent_scroll_y = parent_scroll;
            g_state.mode = LibraryBrowseMode::GroupTracks;
            g_state.detail_type = row->action == LibraryRowAction::OpenArtist
                ? PlayerListType::Artist
                : (row->action == LibraryRowAction::OpenAlbum ? PlayerListType::Album : PlayerListType::Decade);
            g_state.detail_group_index = row->group_index;
            g_state.detail_scroll_y = 0;
            library_view_schedule_render();
            return;
        }

        case LibraryRowAction::PlayAllTrack:
            if (!player_control_select_all_tracks(row->position)) {
                if (transition_hold) player_home_cancel_track_transition_hold();
                ESP_LOGW(TAG, "选择全部歌曲位置失败：%lu", static_cast<unsigned long>(row->position));
                return;
            }
            break;

        case LibraryRowAction::PlayFolderTrack:
            if (!player_control_select_folder_queue_position(row->position)) {
                if (transition_hold) player_home_cancel_track_transition_hold();
                ESP_LOGW(TAG, "选择目录列表位置失败：%lu", static_cast<unsigned long>(row->position));
                return;
            }
            break;

        case LibraryRowAction::PlayGroupTrack:
        {
            bool selected = false;
            switch (row->group_type) {
                case PlayerListType::Artist:
                    selected = player_control_select_artist_group(row->group_index, row->position);
                    break;
                case PlayerListType::Album:
                    selected = player_control_select_album_group(row->group_index, row->position);
                    break;
                case PlayerListType::Decade:
                    selected = player_control_select_decade_group(row->group_index, row->position);
                    break;
                case PlayerListType::AllTracks:
                    selected = player_control_select_all_tracks(row->position);
                    break;
            }
            if (!selected) {
                if (transition_hold) player_home_cancel_track_transition_hold();
                ESP_LOGW(TAG, "选择分组歌曲失败：类型=%s group=%lu pos=%lu",
                    player_playlist_type_name(row->group_type),
                    static_cast<unsigned long>(row->group_index),
                    static_cast<unsigned long>(row->position));
                return;
            }
            break;
        }


        case LibraryRowAction::SelectLevel1Folder:
        {
            char path[PLAYER_FOLDER_PATH_MAX] = {};
            if (!player_playlist_copy_folder_option_at(
                    PlayerFolderScope::Level1, nullptr, row->position,
                    path, sizeof(path), nullptr) ||
                !library_view_persist_folder_selection(PlayerFolderScope::Level1, path)) {
                ESP_LOGW(TAG, "一级文件夹切换失败：pos=%lu",
                    static_cast<unsigned long>(row->position));
                return;
            }
            g_state.folder_view = LibraryFolderView::Tracks;
            PlayerFolderQueueSnapshot queue = {};
            g_state.top_scroll_y[0] = library_view_folder_queue_snapshot(&queue) && queue.current_in_queue
                ? library_view_scroll_for_position(queue.position) : 0;
            library_view_schedule_render();
            UI_PAGE_INTERACTION_LOGI("一级列表文件夹已切换：%s", path);
            return;
        }

        case LibraryRowAction::SelectLevel2Folder:
        {
            if (!library_view_resolve_level2_parent()) return;
            char path[PLAYER_FOLDER_PATH_MAX] = {};
            if (!player_playlist_copy_folder_option_at(
                    PlayerFolderScope::Level2, g_state.level2_parent_path, row->position,
                    path, sizeof(path), nullptr) ||
                !library_view_persist_folder_selection(PlayerFolderScope::Level2, path)) {
                ESP_LOGW(TAG, "二级文件夹切换失败：parent=%s pos=%lu",
                    g_state.level2_parent_path,
                    static_cast<unsigned long>(row->position));
                return;
            }
            g_state.folder_view = LibraryFolderView::Tracks;
            PlayerFolderQueueSnapshot queue = {};
            g_state.top_scroll_y[0] = library_view_folder_queue_snapshot(&queue) && queue.current_in_queue
                ? library_view_scroll_for_position(queue.position) : 0;
            library_view_schedule_render();
            UI_PAGE_INTERACTION_LOGI("二级列表文件夹已切换：%s", path);
            return;
        }

        case LibraryRowAction::SelectLevel2Parent:
        {
            char path[PLAYER_FOLDER_PATH_MAX] = {};
            if (!player_playlist_copy_folder_option_at(
                    PlayerFolderScope::Level1, nullptr, row->position,
                    path, sizeof(path), nullptr)) {
                return;
            }
            snprintf(g_state.level2_parent_path, sizeof(g_state.level2_parent_path), "%s", path);
            g_state.folder_view = LibraryFolderView::Level2Folders;
            g_state.folder_scroll_y[1] = 0;
            library_view_schedule_render();
            UI_PAGE_INTERACTION_LOGI("二级目录父级切换：%s", path);
            return;
        }

        case LibraryRowAction::FolderUp:
            g_state.folder_view = LibraryFolderView::Level2Parents;
            g_state.folder_scroll_y[2] = 0;
            library_view_schedule_render();
            UI_PAGE_INTERACTION_LOGI("二级文件夹：进入上一级目录列表");
            return;

        case LibraryRowAction::None:
            return;
    }

    if (!player_control_play_current()) {
        ESP_LOGW(TAG, "曲库选歌后播放请求未能入队");
    }
    // R.36.2：必须先隐藏曲库再恢复主页；旧顺序先 refresh 时 QoS 仍看到曲库 visible，
    // 会让 Artwork 保持 inactive，切歌返回后存在黑屏/延迟恢复窗口。
    if (g_root != nullptr) {
        lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    }
    player_home_resume_from_fullscreen_view("library-select");
}

static void library_view_return_to_parent_category()
{
    if (g_state.mode != LibraryBrowseMode::GroupTracks) {
        return;
    }

    g_state.mode = library_view_category_mode_for_detail();
    g_state.detail_type = PlayerListType::AllTracks;
    g_state.detail_group_index = UINT32_MAX;
    const uint8_t mode = static_cast<uint8_t>(g_state.mode);
    if (mode < 4U) {
        g_state.top_scroll_y[mode] = g_state.parent_scroll_y;
    }
    g_state.detail_scroll_y = 0;
    library_view_render(true);
}

static void library_view_back_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || g_root == nullptr || library_click_suppressed()) {
        return;
    }
    if (g_search.active) {
        library_search_exit();
        return;
    }
    if (library_view_folder_browser_active()) {
        if (g_state.folder_view == LibraryFolderView::Level2Parents) {
            g_state.folder_view = LibraryFolderView::Level2Folders;
        } else {
            g_state.folder_view = LibraryFolderView::Tracks;
        }
        library_view_render(true);
        return;
    }
    if (g_state.mode != LibraryBrowseMode::GroupTracks) {
        library_view_close_to_home();
        return;
    }

    library_view_return_to_parent_category();
}

static void library_view_search_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || library_click_suppressed()) {
        return;
    }
    if (g_search.active) {
        library_search_exit();
    } else {
        library_search_enter();
    }
}

static void library_view_create_virtual_rows()
{
    for (uint32_t slot = 0U; slot < LIBRARY_VIRTUAL_ROWS; ++slot) {
        LibraryVirtualRow &row = g_rows[slot];
        row.button = lv_button_create(g_list_host);
        ui_common_lock_object(row.button);
        lv_obj_set_pos(row.button, LIBRARY_ROW_X, 4 + static_cast<int32_t>(slot) * LIBRARY_ROW_STEP);
        lv_obj_set_size(row.button, LIBRARY_ROW_W, LIBRARY_ROW_H);
        lv_obj_set_style_radius(row.button, 11, 0);
        lv_obj_set_style_bg_color(row.button, lv_color_hex(0x151A21), 0);
        lv_obj_set_style_bg_opa(row.button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row.button, 0, 0);
        lv_obj_set_style_shadow_width(row.button, 0, 0);
        lv_obj_set_style_pad_all(row.button, 0, 0);
        lv_obj_add_event_cb(row.button, library_view_row_clicked_cb, LV_EVENT_CLICKED, &row.binding);

        row.accent = lv_obj_create(row.button);
        ui_common_lock_object(row.accent);
        lv_obj_set_pos(row.accent, 0, 12);
        lv_obj_set_size(row.accent, 3, 38);
        lv_obj_set_style_radius(row.accent, 2, 0);
        lv_obj_set_style_bg_color(row.accent, lv_color_hex(0xF2F3F5), 0);
        lv_obj_set_style_bg_opa(row.accent, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row.accent, 0, 0);
        lv_obj_add_flag(row.accent, LV_OBJ_FLAG_HIDDEN);

        row.label = library_view_create_label(row.button, "", lv_color_hex(0xE9ECF1), font_manager_get_ui_font());
        lv_label_set_long_mode(row.label, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(row.label, 18, 15);
        lv_obj_set_size(row.label, LIBRARY_ROW_W - 36, 32);
        lv_obj_set_style_text_align(row.label, LV_TEXT_ALIGN_LEFT, 0);

        row.meta = library_view_create_label(
            row.button, "", lv_color_hex(0x98A3B2), font_manager_get_ui_font());
        lv_obj_set_pos(row.meta, LIBRARY_ROW_W - 116, 15);
        lv_obj_set_size(row.meta, 54, 32);
        lv_obj_set_style_text_align(row.meta, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(row.meta, LV_OBJ_FLAG_HIDDEN);

        row.arrow = library_view_create_label(row.button, LV_SYMBOL_RIGHT, lv_color_hex(0x697382), lv_font_default());
        lv_obj_align(row.arrow, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_add_flag(row.arrow, LV_OBJ_FLAG_HIDDEN);
    }
}

void library_view_create(lv_obj_t *screen)
{
    if (screen == nullptr || g_root != nullptr) {
        return;
    }

    g_root = lv_obj_create(screen);
    ui_common_lock_object(g_root);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_size(g_root, FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT);
    lv_obj_set_style_radius(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x0D1016), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_shadow_width(g_root, 0, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);

    g_back_button = library_view_create_icon_button(
        g_root, LIBRARY_BACK_BUTTON_X, LIBRARY_HEADER_BUTTON_Y,
        LIBRARY_HEADER_BUTTON_W, LIBRARY_HEADER_BUTTON_H, LV_SYMBOL_LEFT);
    lv_obj_add_event_cb(g_back_button, library_view_back_cb, LV_EVENT_CLICKED, nullptr);

    g_header_title = library_view_create_label(g_root, "歌曲", lv_color_hex(0xFFFFFF), font_manager_get_ui_font());
    lv_label_set_long_mode(g_header_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_header_title, LIBRARY_TITLE_X, 17);
    lv_obj_set_size(g_header_title, LIBRARY_TITLE_W, 34);
    lv_obj_set_style_text_align(g_header_title, LV_TEXT_ALIGN_CENTER, 0);

    g_scope_icon = lv_obj_create(g_root);
    ui_common_lock_object(g_scope_icon);
    lv_obj_set_pos(g_scope_icon, LIBRARY_SCOPE_ICON_X, LIBRARY_SCOPE_ICON_Y);
    lv_obj_set_size(g_scope_icon, LIBRARY_SCOPE_ICON_W, LIBRARY_SCOPE_ICON_H);
    lv_obj_set_style_bg_opa(g_scope_icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_scope_icon, 0, 0);
    lv_obj_set_style_pad_all(g_scope_icon, 0, 0);
    lv_obj_clear_flag(g_scope_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_scope_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_scope_icon, LV_OBJ_FLAG_HIDDEN);

    // 二级模式多一层后置文件夹；一级模式只显示前层。
    g_scope_icon_back = lv_obj_create(g_scope_icon);
    ui_common_lock_object(g_scope_icon_back);
    lv_obj_set_pos(g_scope_icon_back, 1, 4);
    lv_obj_set_size(g_scope_icon_back, 14, 11);
    lv_obj_set_style_radius(g_scope_icon_back, 2, 0);
    lv_obj_set_style_bg_opa(g_scope_icon_back, LV_OPA_10, 0);
    lv_obj_set_style_border_width(g_scope_icon_back, 2, 0);
    lv_obj_set_style_pad_all(g_scope_icon_back, 0, 0);

    g_scope_icon_back_tab = lv_obj_create(g_scope_icon);
    ui_common_lock_object(g_scope_icon_back_tab);
    lv_obj_set_pos(g_scope_icon_back_tab, 1, 2);
    lv_obj_set_size(g_scope_icon_back_tab, 7, 5);
    lv_obj_set_style_radius(g_scope_icon_back_tab, 2, 0);
    lv_obj_set_style_bg_opa(g_scope_icon_back_tab, LV_OPA_10, 0);
    lv_obj_set_style_border_width(g_scope_icon_back_tab, 2, 0);
    lv_obj_set_style_pad_all(g_scope_icon_back_tab, 0, 0);

    g_scope_icon_front = lv_obj_create(g_scope_icon);
    ui_common_lock_object(g_scope_icon_front);
    lv_obj_set_pos(g_scope_icon_front, 6, 7);
    lv_obj_set_size(g_scope_icon_front, 15, 11);
    lv_obj_set_style_radius(g_scope_icon_front, 2, 0);
    lv_obj_set_style_bg_opa(g_scope_icon_front, LV_OPA_10, 0);
    lv_obj_set_style_border_width(g_scope_icon_front, 2, 0);
    lv_obj_set_style_pad_all(g_scope_icon_front, 0, 0);

    g_scope_icon_front_tab = lv_obj_create(g_scope_icon);
    ui_common_lock_object(g_scope_icon_front_tab);
    lv_obj_set_pos(g_scope_icon_front_tab, 6, 5);
    lv_obj_set_size(g_scope_icon_front_tab, 7, 5);
    lv_obj_set_style_radius(g_scope_icon_front_tab, 2, 0);
    lv_obj_set_style_bg_opa(g_scope_icon_front_tab, LV_OPA_10, 0);
    lv_obj_set_style_border_width(g_scope_icon_front_tab, 2, 0);
    lv_obj_set_style_pad_all(g_scope_icon_front_tab, 0, 0);

    g_search_button = library_view_create_search_button(
        g_root, LIBRARY_SEARCH_BUTTON_X, LIBRARY_HEADER_BUTTON_Y,
        LIBRARY_HEADER_BUTTON_W, LIBRARY_HEADER_BUTTON_H);
    lv_obj_add_event_cb(g_search_button, library_view_search_cb, LV_EVENT_CLICKED, nullptr);

    static constexpr int32_t INDICATOR_W = 28;
    static constexpr int32_t INDICATOR_GAP = 9;
    static constexpr int32_t INDICATOR_TOTAL_W = INDICATOR_W * 4 + INDICATOR_GAP * 3;
    static constexpr int32_t INDICATOR_X = (FAKEPOD_LCD_WIDTH - INDICATOR_TOTAL_W) / 2;
    for (uint32_t i = 0U; i < 4U; ++i) {
        g_indicator[i] = lv_obj_create(g_root);
        ui_common_lock_object(g_indicator[i]);
        lv_obj_set_pos(g_indicator[i], INDICATOR_X + static_cast<int32_t>(i) * (INDICATOR_W + INDICATOR_GAP), 67);
        lv_obj_set_size(g_indicator[i], INDICATOR_W, 4);
        lv_obj_set_style_radius(g_indicator[i], 2, 0);
        lv_obj_set_style_border_width(g_indicator[i], 0, 0);
        lv_obj_set_style_bg_opa(g_indicator[i], LV_OPA_COVER, 0);
    }

    // P1.3.4.2：列表不再依赖 LVGL 的手势滚动状态机。
    // CST820 原始坐标直接驱动 g_manual_scroll_y；LVGL 只负责显示与轻点事件。
    g_list_host = lv_obj_create(g_root);
    ui_common_lock_object(g_list_host);
    lv_obj_set_pos(g_list_host, LIBRARY_LIST_X, LIBRARY_LIST_Y);
    lv_obj_set_size(g_list_host, LIBRARY_LIST_W, LIBRARY_LIST_H);
    lv_obj_set_style_radius(g_list_host, 0, 0);
    lv_obj_set_style_bg_opa(g_list_host, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_list_host, 0, 0);
    lv_obj_set_style_pad_all(g_list_host, 0, 0);
    lv_obj_set_scrollbar_mode(g_list_host, LV_SCROLLBAR_MODE_OFF);

    library_view_create_virtual_rows();

    g_scrollbar.track = lv_obj_create(g_root);
    ui_common_lock_object(g_scrollbar.track);
    lv_obj_set_style_radius(g_scrollbar.track, 2, 0);
    lv_obj_set_style_border_width(g_scrollbar.track, 0, 0);
    lv_obj_set_style_bg_color(g_scrollbar.track, lv_color_hex(0x405064), 0);
    lv_obj_set_style_bg_opa(g_scrollbar.track, LV_OPA_20, 0);
    lv_obj_clear_flag(g_scrollbar.track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_scrollbar.track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_scrollbar.track, LV_OBJ_FLAG_HIDDEN);

    g_scrollbar.thumb = lv_obj_create(g_root);
    ui_common_lock_object(g_scrollbar.thumb);
    lv_obj_set_style_radius(g_scrollbar.thumb, 2, 0);
    lv_obj_set_style_border_width(g_scrollbar.thumb, 0, 0);
    lv_obj_set_style_bg_color(g_scrollbar.thumb, lv_color_hex(0xAFC3DC), 0);
    lv_obj_set_style_bg_opa(g_scrollbar.thumb, LV_OPA_70, 0);
    lv_obj_clear_flag(g_scrollbar.thumb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_scrollbar.thumb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_scrollbar.thumb, LV_OBJ_FLAG_HIDDEN);


    g_inertia.timer = lv_timer_create(
        library_view_inertia_timer_cb,
        LIBRARY_INERTIA_PERIOD_MS,
        nullptr);
    if (g_inertia.timer != nullptr) {
        lv_timer_pause(g_inertia.timer);
    }

    if (!quick_index_keyboard_create(
            &g_search_keyboard,
            g_root,
            LIBRARY_SEARCH_KEYBOARD_X,
            LIBRARY_SEARCH_KEYBOARD_Y,
            LIBRARY_SEARCH_KEYBOARD_W,
            LIBRARY_SEARCH_KEYBOARD_H,
            library_search_keyboard_cb,
            nullptr)) {
        ESP_LOGE(TAG, "创建2x5快速索引键盘失败");
    } else {
        quick_index_keyboard_set_visible(&g_search_keyboard, false);
    }

    g_hint = library_view_create_label(g_root, "", lv_color_hex(0x9CA5B3), font_manager_get_ui_font());
    lv_obj_set_size(g_hint, 260, 34);
    lv_obj_set_style_text_align(g_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_hint, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_color(g_hint, lv_color_hex(0x171C24), 0);
    lv_obj_set_style_bg_opa(g_hint, LV_OPA_90, 0);
    lv_obj_set_style_pad_left(g_hint, 12, 0);
    lv_obj_set_style_pad_right(g_hint, 12, 0);
    lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    library_view_render(false);

    UI_PAGE_BOOT_LOGI("曲库页：TouchFastPathBackpressure，scrollbar=%dpx",
        static_cast<int>(LIBRARY_SCROLLBAR_W));
}

void library_view_suspend_for_app_switch()
{
    if (g_root == nullptr) {
        return;
    }
    library_view_inertia_stop(false);
    if (lv_obj_has_flag(g_root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    library_view_store_scroll_position();
    library_view_scrollbar_set_visible(false);
    g_gesture = {};
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    UI_PAGE_INTERACTION_LOGI("曲库因APP切换挂起");
}

void library_view_open()
{
    if (g_root == nullptr) {
        return;
    }
    if (!media_catalog_v2_ready()) {
        ESP_LOGW(TAG, "Catalog 尚未就绪，无法打开曲库");
        return;
    }

    library_view_inertia_stop(false);
    library_view_scrollbar_set_visible(false);
    g_state = {};
    g_gesture = {};
    g_search.active = false;
    g_search.key_index = static_cast<uint8_t>(LibrarySearchBucket::All);
    g_search.query_length = 0U;
    g_search.parent_scroll_y = 0;
    g_search.scroll_y = 0;
    g_search.match_count = 0U;
    quick_index_keyboard_set_visible(&g_search_keyboard, false);
    if (library_view_folder_scope_active()) {
        PlayerFolderQueueSnapshot queue = {};
        g_state.mode = LibraryBrowseMode::AllTracks;
        g_state.detail_type = PlayerListType::AllTracks;
        g_state.detail_group_index = UINT32_MAX;
        if (library_view_folder_queue_snapshot(&queue) && queue.track_count > 0U) {
            g_state.top_scroll_y[0] = library_view_scroll_for_position(queue.position);
        }
    } else {
        PlayerListSnapshot playlist = {};
        if (player_state_get_list_snapshot(&playlist) &&
            playlist.catalog_generation == media_catalog_v2_generation() &&
            playlist.track_count > 0U) {
            if (playlist.type == PlayerListType::AllTracks) {
                g_state.mode = LibraryBrowseMode::AllTracks;
                g_state.top_scroll_y[0] = library_view_scroll_for_position(playlist.position);
            } else {
                g_state.mode = LibraryBrowseMode::GroupTracks;
                g_state.detail_type = playlist.type;
                g_state.detail_group_index = playlist.group_index;
                g_state.detail_scroll_y = library_view_scroll_for_position(playlist.position);
                g_state.parent_scroll_y = library_view_scroll_for_position(playlist.group_index);
                uint8_t parent_mode = 0U;
                if (playlist.type == PlayerListType::Artist) parent_mode = static_cast<uint8_t>(LibraryBrowseMode::Artists);
                else if (playlist.type == PlayerListType::Album) parent_mode = static_cast<uint8_t>(LibraryBrowseMode::Albums);
                else if (playlist.type == PlayerListType::Decade) parent_mode = static_cast<uint8_t>(LibraryBrowseMode::Decades);
                if (parent_mode < 4U) {
                    g_state.top_scroll_y[parent_mode] = g_state.parent_scroll_y;
                }
            }
        }
    }

    library_view_render(true);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    screen_lock_simple_raise();
    UI_PAGE_INTERACTION_LOGI("打开曲库：generation=%lu list=%s scroll=%ld",
        static_cast<unsigned long>(media_catalog_v2_generation()),
        player_playlist_type_name(player_state_get_list_type()),
        static_cast<long>(library_view_saved_scroll_position()));
}

bool library_view_is_visible()
{
    return g_root != nullptr && !lv_obj_has_flag(g_root, LV_OBJ_FLAG_HIDDEN);
}

void library_view_feed_pointer(bool pressed, int16_t x, int16_t y, uint32_t tick_ms)
{
    if (!library_view_is_visible()) {
        library_view_inertia_stop(false);
        g_gesture.pressed = false;
        g_gesture.axis = LibraryGestureAxis::None;
        return;
    }

    if (pressed) {
        if (!g_gesture.pressed) {
            const bool stopped_inertia = g_inertia.active;
            library_view_inertia_stop(true);

            // P1.5R.2.1：点击抑制只能属于“上一轮拖动自身”，不能跨到下一次独立点击。
            // 拖动 MOVE/UP 后仍保留 suppress_click_until，用于拦截 LVGL 为该拖动产生的 CLICK；
            // 一旦收到下一轮新的 DOWN，就清除上一手势残留。若此 DOWN 正好用于刹停列表惯性，
            // 下面会仅对本轮列表按压重新建立 suppression，因此 Header 仍可一次点击生效。
            g_gesture.suppress_click_until = 0U;

            g_gesture.pressed = true;
            g_gesture.start_x = x;
            g_gesture.start_y = y;
            g_gesture.last_x = x;
            g_gesture.last_y = y;
            g_gesture.start_tick_ms = tick_ms;
            g_gesture.last_sample_tick_ms = tick_ms;
            g_gesture.last_motion_tick_ms = tick_ms;
            g_gesture.scroll_velocity_px_s = 0;
            g_gesture.axis = LibraryGestureAxis::None;
            const int32_t list_y = library_view_list_y();
            const int32_t list_h = library_view_list_h();
            g_gesture.started_in_list = y >= list_y && y < list_y + list_h;
            // 点正在惯性滚动的列表：第一下只负责刹车，不顺便触发该行。
            // Header 按钮仍可一次点击生效，不因为列表惯性而被全局吞掉。
            g_gesture.list_dragged = stopped_inertia && g_gesture.started_in_list;
            if (g_gesture.list_dragged) {
                g_gesture.suppress_click_until = tick_ms + LIBRARY_SUPPRESS_CLICK_MS;
            }

            const bool search_area =
                y >= LIBRARY_HEADER_BUTTON_Y &&
                y < LIBRARY_HEADER_BUTTON_Y + LIBRARY_HEADER_BUTTON_H &&
                x >= LIBRARY_SEARCH_BUTTON_X &&
                x < LIBRARY_SEARCH_BUTTON_X + LIBRARY_HEADER_BUTTON_W;
            const bool back_area =
                y >= LIBRARY_HEADER_BUTTON_Y &&
                y < LIBRARY_HEADER_BUTTON_Y + LIBRARY_HEADER_BUTTON_H &&
                x >= LIBRARY_BACK_BUTTON_X &&
                x < LIBRARY_BACK_BUTTON_X + LIBRARY_HEADER_BUTTON_W;
            const bool keyboard_area = quick_index_keyboard_contains_point(&g_search_keyboard, x, y);
            g_gesture.ignore = search_area || back_area || keyboard_area;
            return;
        }

        const int16_t previous_y = g_gesture.last_y;
        const int32_t dx = static_cast<int32_t>(x) - g_gesture.start_x;
        const int32_t dy = static_cast<int32_t>(y) - g_gesture.start_y;
        const int32_t ax = library_abs(dx);
        const int32_t ay = library_abs(dy);

        if (!g_gesture.ignore) {
            // 6px 即锁方向。列表里只要纵向意图更明显，就立即进入直驱滚动，
            // 不再等待 LVGL 判断 SCROLL_BEGIN。
            if (g_gesture.axis == LibraryGestureAxis::None &&
                (ax >= LIBRARY_AXIS_LOCK_PX || ay >= LIBRARY_AXIS_LOCK_PX)) {
                g_gesture.axis = (ax > ay) ? LibraryGestureAxis::Horizontal : LibraryGestureAxis::Vertical;
            }

            if (g_gesture.started_in_list && !g_gesture.list_dragged &&
                (ax >= LIBRARY_LIST_DRAG_GUARD_PX || ay >= LIBRARY_LIST_DRAG_GUARD_PX)) {
                g_gesture.list_dragged = true;
                g_gesture.suppress_click_until = tick_ms + LIBRARY_SUPPRESS_CLICK_MS;
            }

            if (g_gesture.started_in_list &&
                g_gesture.axis == LibraryGestureAxis::Vertical) {
                const int32_t step_y = static_cast<int32_t>(y) - previous_y;
                if (step_y != 0) {
                    // 手指向上 => scroll_y 增大；手指向下 => scroll_y 减小。
                    const int32_t before = library_view_current_scroll_y();
                    library_view_set_scroll_y(before - step_y);
                    const int32_t after = library_view_current_scroll_y();
                    const int32_t actual_scroll_delta = after - before;
                    const uint32_t sample_dt_ms = tick_ms - g_gesture.last_sample_tick_ms;
                    if (actual_scroll_delta != 0 && sample_dt_ms > 0U) {
                        g_gesture.last_motion_tick_ms = tick_ms;
                        int32_t instantaneous = static_cast<int32_t>(
                            (static_cast<int64_t>(actual_scroll_delta) * 1000LL) /
                            static_cast<int64_t>(sample_dt_ms));
                        instantaneous = library_clamp_i32(
                            instantaneous, -LIBRARY_INERTIA_MAX_PX_S, LIBRARY_INERTIA_MAX_PX_S);
                        if (g_gesture.scroll_velocity_px_s == 0) {
                            g_gesture.scroll_velocity_px_s = instantaneous;
                        } else {
                            // 最近采样权重大，既能跟上快速甩动，又能过滤 CST820 单点抖动。
                            g_gesture.scroll_velocity_px_s =
                                (g_gesture.scroll_velocity_px_s * 2 + instantaneous * 3) / 5;
                        }
                    } else if (actual_scroll_delta == 0) {
                        // 已经触顶/触底时不保留向外的速度，防止抬手后继续“顶边”。
                        g_gesture.scroll_velocity_px_s = 0;
                    }
                    g_gesture.suppress_click_until = tick_ms + LIBRARY_SUPPRESS_CLICK_MS;
                }
                g_gesture.last_sample_tick_ms = tick_ms;
            }
        }

        g_gesture.last_x = x;
        g_gesture.last_y = y;
        return;
    }

    if (!g_gesture.pressed) {
        return;
    }

    g_gesture.last_x = x;
    g_gesture.last_y = y;

    LibraryPendingGesture action = LibraryPendingGesture::None;
    if (!g_gesture.ignore) {
        const int32_t dx = static_cast<int32_t>(g_gesture.last_x) - g_gesture.start_x;
        const int32_t dy = static_cast<int32_t>(g_gesture.last_y) - g_gesture.start_y;
        const int32_t ax = library_abs(dx);
        const int32_t ay = library_abs(dy);
        const uint32_t elapsed = tick_ms - g_gesture.start_tick_ms;

        if (g_gesture.started_in_list && !g_gesture.list_dragged &&
            (ax >= LIBRARY_LIST_DRAG_GUARD_PX || ay >= LIBRARY_LIST_DRAG_GUARD_PX)) {
            g_gesture.list_dragged = true;
            g_gesture.suppress_click_until = tick_ms + LIBRARY_SUPPRESS_CLICK_MS;
        }

        if (g_gesture.axis == LibraryGestureAxis::None &&
            (ax >= LIBRARY_AXIS_LOCK_PX || ay >= LIBRARY_AXIS_LOCK_PX)) {
            g_gesture.axis = (ax > ay) ? LibraryGestureAxis::Horizontal : LibraryGestureAxis::Vertical;
        }

        if (g_gesture.axis == LibraryGestureAxis::Horizontal &&
            !g_search.active &&
            ax >= LIBRARY_HORIZONTAL_TRIGGER_PX &&
            ax * 100 >= ay * 135 &&
            library_speed_ok(ax, elapsed, LIBRARY_HORIZONTAL_MIN_SPEED)) {
            if (g_state.mode == LibraryBrowseMode::GroupTracks) {
                // 详情页绝不跟手横移：右滑只作为“返回父级”的离散手势，左滑无动作。
                if (dx > 0) {
                    action = LibraryPendingGesture::SwipeRight;
                }
            } else {
                // 顶层仍保留四分类的离散左右切换，同样不移动整页对象。
                action = dx < 0 ? LibraryPendingGesture::SwipeLeft : LibraryPendingGesture::SwipeRight;
            }
        }
    }

    const bool start_inertia =
        !g_gesture.ignore &&
        action == LibraryPendingGesture::None &&
        g_gesture.started_in_list &&
        g_gesture.list_dragged &&
        g_gesture.axis == LibraryGestureAxis::Vertical &&
        (tick_ms - g_gesture.last_motion_tick_ms) <= 120U &&
        library_abs(g_gesture.scroll_velocity_px_s) >= LIBRARY_INERTIA_START_MIN_PX_S;
    const int32_t release_velocity = g_gesture.scroll_velocity_px_s;

    // 抬手时先保存当前位置；若启动惯性，最终停止时会再保存一次最终位置。
    if (g_gesture.started_in_list && g_gesture.list_dragged) {
        library_view_store_scroll_position();
    }

    g_gesture.pressed = false;
    g_gesture.axis = LibraryGestureAxis::None;
    g_gesture.ignore = false;
    g_gesture.started_in_list = false;
    g_gesture.list_dragged = false;
    g_gesture.scroll_velocity_px_s = 0;
    g_gesture.last_sample_tick_ms = 0U;
    g_gesture.last_motion_tick_ms = 0U;

    // Direct Touch 已结束：辅助位置条立即追到最终位置，不把节流残留到抬手后。
    library_view_scrollbar_update_position();

    if (start_inertia) {
        library_view_inertia_start(release_velocity, tick_ms);
    }
    library_view_queue_gesture(action, tick_ms);
}


bool library_view_inertia_is_active()
{
    return g_inertia.active;
}

bool library_view_search_is_active()
{
    return g_search.active;
}
