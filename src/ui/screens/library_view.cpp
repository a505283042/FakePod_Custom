#include "library_view.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "board_pins.h"
#include "font/font_manager.h"
#include "media_catalog_v2.h"
#include "media_groups_v2.h"
#include "media_library.h"
#include "player_control.h"
#include "player_home.h"
#include "player_playlist.h"
#include "player_state.h"
#include "ui_common.h"

static const char *TAG = "曲库界面";

static constexpr uint32_t LIBRARY_ROWS_PER_PAGE = 5U;
static constexpr int32_t LIBRARY_ROW_X = 16;
static constexpr int32_t LIBRARY_ROW_Y = 104;
static constexpr int32_t LIBRARY_ROW_W = 428;
static constexpr int32_t LIBRARY_ROW_H = 50;
static constexpr int32_t LIBRARY_ROW_STEP = 54;

// 四个顶层视图与 PlayerListType 对齐，但保持 UI 自己的状态，避免把“浏览页面”误当播放上下文。
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
    OpenArtist,
    OpenAlbum,
    OpenDecade,
    PlayGroupTrack,
};

struct LibraryRowBinding
{
    LibraryRowAction action = LibraryRowAction::None;
    PlayerListType group_type = PlayerListType::AllTracks;
    uint32_t generation = 0;
    uint32_t group_index = UINT32_MAX;
    uint32_t position = 0;
};

struct LibraryBrowseState
{
    LibraryBrowseMode mode = LibraryBrowseMode::AllTracks;
    PlayerListType detail_type = PlayerListType::AllTracks;
    uint32_t detail_group_index = UINT32_MAX;
    uint32_t page = 0;
    uint32_t parent_page = 0;
};

static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_header = nullptr;
static lv_obj_t *g_list_host = nullptr;
static lv_obj_t *g_page_label = nullptr;
static lv_obj_t *g_prev_page = nullptr;
static lv_obj_t *g_next_page = nullptr;
static lv_obj_t *g_tabs[4] = {};
static LibraryRowBinding g_row_bindings[LIBRARY_ROWS_PER_PAGE] = {};
static LibraryBrowseState g_state = {};

static void library_view_render();

static void library_view_render_async(void *user_data)
{
    (void)user_data;
    library_view_render();
}

static void library_view_schedule_render()
{
    // 分组行的点击回调来自即将被清理的 list row；延后到当前事件结束后再重建列表。
    lv_async_call(library_view_render_async, nullptr);
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
    lv_obj_set_style_text_font(label, font != nullptr ? font : lv_font_default(), 0);
    return label;
}

static lv_obj_t *library_view_create_button(
    lv_obj_t *parent,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const char *text)
{
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1B2029), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);

    lv_obj_t *label = library_view_create_label(
        button, text, lv_color_hex(0xE9ECF1), font_manager_get_ui_font());
    lv_obj_center(label);
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

static void library_view_apply_tab_styles()
{
    const LibraryBrowseMode active = library_view_category_mode_for_detail();
    for (uint32_t i = 0; i < 4U; ++i) {
        if (g_tabs[i] == nullptr) {
            continue;
        }
        const bool selected = static_cast<uint8_t>(active) == i;
        lv_obj_set_style_bg_color(
            g_tabs[i],
            lv_color_hex(selected ? 0xF2F3F5 : 0x1B2029),
            0);
        lv_obj_t *label = lv_obj_get_child(g_tabs[i], 0);
        if (label != nullptr) {
            lv_obj_set_style_text_color(
                label,
                lv_color_hex(selected ? 0x11151B : 0xAAB2BF),
                0);
        }
    }
}

static uint32_t library_view_top_level_count(LibraryBrowseMode mode)
{
    switch (mode) {
        case LibraryBrowseMode::AllTracks:
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

static uint32_t library_view_item_count()
{
    if (g_state.mode != LibraryBrowseMode::GroupTracks) {
        return library_view_top_level_count(g_state.mode);
    }
    uint32_t track_count = 0U;
    return library_view_get_detail_identity(nullptr, &track_count, nullptr) ? track_count : 0U;
}

static uint32_t library_view_page_count(uint32_t item_count)
{
    if (item_count == 0U) {
        return 1U;
    }
    return (item_count + LIBRARY_ROWS_PER_PAGE - 1U) / LIBRARY_ROWS_PER_PAGE;
}

static void library_view_set_header()
{
    if (g_header == nullptr) {
        return;
    }

    if (g_state.mode == LibraryBrowseMode::GroupTracks) {
        const char *title = nullptr;
        if (library_view_get_detail_identity(&title, nullptr, nullptr) && title != nullptr && title[0] != '\0') {
            lv_label_set_text(g_header, title);
            return;
        }
        lv_label_set_text(g_header, "曲库详情");
        return;
    }

    switch (g_state.mode) {
        case LibraryBrowseMode::AllTracks: lv_label_set_text(g_header, "全部歌曲"); break;
        case LibraryBrowseMode::Artists: lv_label_set_text(g_header, "歌手"); break;
        case LibraryBrowseMode::Albums: lv_label_set_text(g_header, "专辑"); break;
        case LibraryBrowseMode::Decades: lv_label_set_text(g_header, "年代"); break;
        case LibraryBrowseMode::GroupTracks: break;
    }
}

static void library_view_add_row_labels(
    lv_obj_t *button,
    const char *primary,
    const char *secondary,
    bool current_track,
    bool show_arrow)
{
    lv_obj_t *primary_label = library_view_create_label(
        button,
        primary != nullptr && primary[0] != '\0' ? primary : "未知",
        lv_color_hex(current_track ? 0xFFFFFF : 0xE9ECF1),
        font_manager_get_ui_font());
    lv_label_set_long_mode(primary_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(primary_label, 12, 1);
    lv_obj_set_size(primary_label, show_arrow ? 350 : 380, 27);

    lv_obj_t *secondary_label = library_view_create_label(
        button,
        secondary != nullptr ? secondary : "",
        lv_color_hex(0x7E8795),
        font_manager_get_ui_font());
    lv_label_set_long_mode(secondary_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(secondary_label, 12, 26);
    lv_obj_set_size(secondary_label, show_arrow ? 350 : 390, 22);

    if (show_arrow) {
        lv_obj_t *arrow = library_view_create_label(
            button, ">", lv_color_hex(0x697382), lv_font_default());
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -14, 0);
    }
}

static lv_obj_t *library_view_create_row(uint32_t slot, LibraryRowBinding *binding)
{
    lv_obj_t *button = lv_button_create(g_list_host);
    ui_common_lock_object(button);
    lv_obj_set_pos(button, LIBRARY_ROW_X, LIBRARY_ROW_Y + static_cast<int32_t>(slot) * LIBRARY_ROW_STEP);
    lv_obj_set_size(button, LIBRARY_ROW_W, LIBRARY_ROW_H);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x171C24), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, [](lv_event_t *event) {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
            return;
        }
        LibraryRowBinding *row = static_cast<LibraryRowBinding *>(lv_event_get_user_data(event));
        if (row == nullptr || row->generation == 0U || row->generation != media_catalog_v2_generation()) {
            ESP_LOGW(TAG, "点击行已过期，刷新曲库视图");
            library_view_schedule_render();
            return;
        }

        switch (row->action) {
            case LibraryRowAction::OpenArtist:
                g_state.mode = LibraryBrowseMode::GroupTracks;
                g_state.detail_type = PlayerListType::Artist;
                g_state.detail_group_index = row->group_index;
                g_state.parent_page = g_state.page;
                g_state.page = 0U;
                library_view_schedule_render();
                return;
            case LibraryRowAction::OpenAlbum:
                g_state.mode = LibraryBrowseMode::GroupTracks;
                g_state.detail_type = PlayerListType::Album;
                g_state.detail_group_index = row->group_index;
                g_state.parent_page = g_state.page;
                g_state.page = 0U;
                library_view_schedule_render();
                return;
            case LibraryRowAction::OpenDecade:
                g_state.mode = LibraryBrowseMode::GroupTracks;
                g_state.detail_type = PlayerListType::Decade;
                g_state.detail_group_index = row->group_index;
                g_state.parent_page = g_state.page;
                g_state.page = 0U;
                library_view_schedule_render();
                return;
            case LibraryRowAction::PlayAllTrack:
                if (!player_control_select_all_tracks(row->position)) {
                    ESP_LOGW(TAG, "选择全部歌曲位置失败：%lu", static_cast<unsigned long>(row->position));
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
                    ESP_LOGW(TAG, "选择分组歌曲失败：类型=%s group=%lu pos=%lu",
                        player_playlist_type_name(row->group_type),
                        static_cast<unsigned long>(row->group_index),
                        static_cast<unsigned long>(row->position));
                    return;
                }
                break;
            }
            case LibraryRowAction::None:
                return;
        }

        // 点歌曲即进入对应 Playback List Context 并开始播放。播放失败仍保留已选歌曲，回首页显示真实状态。
        if (!player_control_toggle_play_pause()) {
            ESP_LOGW(TAG, "曲库选歌后播放请求未能入队");
        }
        player_home_refresh();
        if (g_root != nullptr) {
            lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_CLICKED, binding);
    return button;
}

static void library_view_render_track_row(
    uint32_t slot,
    uint32_t track_index,
    LibraryRowBinding *binding,
    bool detail_row)
{
    MediaTrackViewV2 track = {};
    if (!media_catalog_v2_get_track_view(track_index, &track) || track.row == nullptr) {
        return;
    }

    lv_obj_t *button = library_view_create_row(slot, binding);
    const bool current = player_state_is_ready() && player_state_get_index() == track_index;
    if (current) {
        lv_obj_set_style_bg_color(button, lv_color_hex(0x252C37), 0);
    }

    char numeric_secondary[64] = {};
    const char *secondary = nullptr;
    if (detail_row && g_state.detail_type == PlayerListType::Album) {
        const bool has_disc = (track.row->metadata_flags & MEDIA_TRACK_META_HAS_DISC_NUMBER_V2) != 0U;
        const bool has_track = (track.row->metadata_flags & MEDIA_TRACK_META_HAS_TRACK_NUMBER_V2) != 0U;
        if (has_disc && has_track) {
            snprintf(numeric_secondary, sizeof(numeric_secondary), "Disc %u · Track %u",
                static_cast<unsigned>(track.row->disc_number),
                static_cast<unsigned>(track.row->track_number));
            secondary = numeric_secondary;
        } else if (has_track) {
            snprintf(numeric_secondary, sizeof(numeric_secondary), "Track %u", static_cast<unsigned>(track.row->track_number));
            secondary = numeric_secondary;
        } else if (track.artist != nullptr && track.artist[0] != '\0') {
            secondary = track.artist;
        }
    } else if (track.artist != nullptr && track.artist[0] != '\0') {
        secondary = track.artist;
    } else if (track.album != nullptr && track.album[0] != '\0') {
        secondary = track.album;
    } else {
        secondary = media_format_name(track.row->format);
    }

    library_view_add_row_labels(button, track.title, secondary, current, false);
}

static void library_view_render_top_row(uint32_t slot, uint32_t item_index, LibraryRowBinding *binding)
{
    const uint32_t generation = media_catalog_v2_generation();
    binding->generation = generation;
    binding->group_index = item_index;

    if (g_state.mode == LibraryBrowseMode::AllTracks) {
        binding->action = LibraryRowAction::PlayAllTrack;
        binding->group_type = PlayerListType::AllTracks;
        binding->position = item_index;
        library_view_render_track_row(slot, item_index, binding, false);
        return;
    }

    lv_obj_t *button = library_view_create_row(slot, binding);
    char secondary[128] = {};
    const char *primary = "未知";

    if (g_state.mode == LibraryBrowseMode::Artists) {
        MediaArtistGroupViewV2 view = {};
        if (!media_groups_v2_get_artist(item_index, &view)) {
            lv_obj_delete(button);
            return;
        }
        binding->generation = view.generation;
        binding->action = LibraryRowAction::OpenArtist;
        binding->group_type = PlayerListType::Artist;
        primary = view.name;
        snprintf(secondary, sizeof(secondary), "%lu 首", static_cast<unsigned long>(view.track_count));
    } else if (g_state.mode == LibraryBrowseMode::Albums) {
        MediaAlbumGroupViewV2 view = {};
        if (!media_groups_v2_get_album(item_index, &view)) {
            lv_obj_delete(button);
            return;
        }
        binding->generation = view.generation;
        binding->action = LibraryRowAction::OpenAlbum;
        binding->group_type = PlayerListType::Album;
        primary = view.title;
        if (view.artist != nullptr && view.artist[0] != '\0') {
            // Artist 直接引用 StringPool；lv_label_set_text 会复制文本，避免固定缓冲截断 UTF-8。
            library_view_add_row_labels(button, primary, view.artist, false, true);
            return;
        }
        snprintf(secondary, sizeof(secondary), "%lu 首", static_cast<unsigned long>(view.track_count));
    } else if (g_state.mode == LibraryBrowseMode::Decades) {
        MediaDecadeGroupViewV2 view = {};
        if (!media_groups_v2_get_decade(item_index, &view)) {
            lv_obj_delete(button);
            return;
        }
        binding->generation = view.generation;
        binding->action = LibraryRowAction::OpenDecade;
        binding->group_type = PlayerListType::Decade;
        static char decade_labels[LIBRARY_ROWS_PER_PAGE][32] = {};
        if (view.unknown) {
            snprintf(decade_labels[slot], sizeof(decade_labels[slot]), "未知年代");
        } else {
            snprintf(decade_labels[slot], sizeof(decade_labels[slot]), "%u年代", static_cast<unsigned>(view.decade_start));
        }
        primary = decade_labels[slot];
        snprintf(secondary, sizeof(secondary), "%lu 首", static_cast<unsigned long>(view.track_count));
    }

    library_view_add_row_labels(button, primary, secondary, false, true);
}

static void library_view_render_detail_row(uint32_t slot, uint32_t position, LibraryRowBinding *binding)
{
    uint32_t track_index = UINT32_MAX;
    uint32_t generation = 0U;
    if (!library_view_detail_track_index(position, &track_index, &generation)) {
        return;
    }
    binding->action = LibraryRowAction::PlayGroupTrack;
    binding->group_type = g_state.detail_type;
    binding->generation = generation;
    binding->group_index = g_state.detail_group_index;
    binding->position = position;
    library_view_render_track_row(slot, track_index, binding, true);
}

static void library_view_render()
{
    if (g_root == nullptr || g_list_host == nullptr) {
        return;
    }

    library_view_apply_tab_styles();
    library_view_set_header();

    lv_obj_clean(g_list_host);
    for (LibraryRowBinding &binding : g_row_bindings) {
        binding = {};
    }

    uint32_t item_count = library_view_item_count();
    uint32_t page_count = library_view_page_count(item_count);
    if (g_state.page >= page_count) {
        g_state.page = page_count - 1U;
    }

    const uint32_t first = g_state.page * LIBRARY_ROWS_PER_PAGE;
    const uint32_t remaining = item_count > first ? item_count - first : 0U;
    const uint32_t visible = remaining > LIBRARY_ROWS_PER_PAGE ? LIBRARY_ROWS_PER_PAGE : remaining;

    for (uint32_t slot = 0U; slot < visible; ++slot) {
        const uint32_t item_index = first + slot;
        if (g_state.mode == LibraryBrowseMode::GroupTracks) {
            library_view_render_detail_row(slot, item_index, &g_row_bindings[slot]);
        } else {
            library_view_render_top_row(slot, item_index, &g_row_bindings[slot]);
        }
    }

    if (visible == 0U) {
        lv_obj_t *empty = library_view_create_label(
            g_list_host, "暂无内容", lv_color_hex(0x7E8795), font_manager_get_ui_font());
        lv_obj_set_size(empty, 260, 32);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 10);
    }

    if (g_page_label != nullptr) {
        lv_label_set_text_fmt(g_page_label, "%lu / %lu · %lu项",
            static_cast<unsigned long>(g_state.page + 1U),
            static_cast<unsigned long>(page_count),
            static_cast<unsigned long>(item_count));
    }
    if (g_prev_page != nullptr) {
        if (g_state.page == 0U) lv_obj_add_state(g_prev_page, LV_STATE_DISABLED);
        else lv_obj_remove_state(g_prev_page, LV_STATE_DISABLED);
    }
    if (g_next_page != nullptr) {
        if (g_state.page + 1U >= page_count) lv_obj_add_state(g_next_page, LV_STATE_DISABLED);
        else lv_obj_remove_state(g_next_page, LV_STATE_DISABLED);
    }
}

static void library_view_back_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || g_root == nullptr) {
        return;
    }
    if (g_state.mode == LibraryBrowseMode::GroupTracks) {
        g_state.mode = library_view_category_mode_for_detail();
        const uint32_t return_page = g_state.parent_page;
        g_state.detail_type = PlayerListType::AllTracks;
        g_state.detail_group_index = UINT32_MAX;
        g_state.page = return_page;
        g_state.parent_page = 0U;
        library_view_render();
        return;
    }
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    player_home_refresh();
}

static void library_view_tab_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    const uintptr_t raw_mode = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (raw_mode > static_cast<uintptr_t>(LibraryBrowseMode::Decades)) {
        return;
    }
    g_state.mode = static_cast<LibraryBrowseMode>(raw_mode);
    g_state.detail_type = PlayerListType::AllTracks;
    g_state.detail_group_index = UINT32_MAX;
    g_state.page = 0U;
    g_state.parent_page = 0U;
    library_view_render();
}

static void library_view_prev_page_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || g_state.page == 0U) {
        return;
    }
    g_state.page--;
    library_view_render();
}

static void library_view_next_page_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    const uint32_t page_count = library_view_page_count(library_view_item_count());
    if (g_state.page + 1U >= page_count) {
        return;
    }
    g_state.page++;
    library_view_render();
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
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x0E1117), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_shadow_width(g_root, 0, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);

    // list_host 自己不滚动，只作为固定 5 行的绘制容器；翻页由底部按钮完成。
    // 先创建它，再创建 header/tabs，天然保证行容器位于导航控件下方，不依赖 z-order 移动 API。
    g_list_host = lv_obj_create(g_root);
    ui_common_lock_object(g_list_host);
    lv_obj_set_pos(g_list_host, 0, 0);
    lv_obj_set_size(g_list_host, FAKEPOD_LCD_WIDTH, 380);
    lv_obj_set_style_bg_opa(g_list_host, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_list_host, 0, 0);
    lv_obj_set_style_pad_all(g_list_host, 0, 0);

    lv_obj_t *back = library_view_create_button(g_root, 16, 12, 44, 38, "<");
    lv_obj_add_event_cb(back, library_view_back_cb, LV_EVENT_CLICKED, nullptr);

    g_header = library_view_create_label(
        g_root, "全部歌曲", lv_color_hex(0xFFFFFF), font_manager_get_ui_font());
    lv_label_set_long_mode(g_header, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_header, 72, 14);
    lv_obj_set_size(g_header, 340, 32);
    lv_obj_set_style_text_align(g_header, LV_TEXT_ALIGN_LEFT, 0);

    static constexpr const char *TAB_LABELS[4] = {"歌曲", "歌手", "专辑", "年代"};
    for (uint32_t i = 0U; i < 4U; ++i) {
        g_tabs[i] = library_view_create_button(
            g_root,
            16 + static_cast<int32_t>(i) * 108,
            58,
            100,
            38,
            TAB_LABELS[i]);
        lv_obj_add_event_cb(
            g_tabs[i],
            library_view_tab_cb,
            LV_EVENT_CLICKED,
            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }

    g_prev_page = library_view_create_button(g_root, 16, 402, 72, 42, "<");
    lv_obj_add_event_cb(g_prev_page, library_view_prev_page_cb, LV_EVENT_CLICKED, nullptr);

    g_page_label = library_view_create_label(
        g_root, "1 / 1", lv_color_hex(0x7E8795), font_manager_get_ui_font());
    lv_obj_set_pos(g_page_label, 100, 408);
    lv_obj_set_size(g_page_label, 260, 30);
    lv_obj_set_style_text_align(g_page_label, LV_TEXT_ALIGN_CENTER, 0);

    g_next_page = library_view_create_button(g_root, 372, 402, 72, 42, ">");
    lv_obj_add_event_cb(g_next_page, library_view_next_page_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    library_view_render();

    ESP_LOGI(TAG, "Stage 10.6 曲库视图已创建：分页=%u 行/页，歌曲=%u 歌手=%u 专辑=%u 年代=%u",
        static_cast<unsigned>(LIBRARY_ROWS_PER_PAGE),
        static_cast<unsigned>(media_library_get_count()),
        static_cast<unsigned>(media_groups_v2_artist_count()),
        static_cast<unsigned>(media_groups_v2_album_count()),
        static_cast<unsigned>(media_groups_v2_decade_count()));
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
    g_state = {};
    PlayerListSnapshot playlist = {};
    if (player_state_get_list_snapshot(&playlist) &&
        playlist.catalog_generation == media_catalog_v2_generation() &&
        playlist.track_count > 0U) {
        if (playlist.type == PlayerListType::AllTracks) {
            g_state.mode = LibraryBrowseMode::AllTracks;
            g_state.page = playlist.position / LIBRARY_ROWS_PER_PAGE;
        } else {
            g_state.mode = LibraryBrowseMode::GroupTracks;
            g_state.detail_type = playlist.type;
            g_state.detail_group_index = playlist.group_index;
            g_state.page = playlist.position / LIBRARY_ROWS_PER_PAGE;
        }
    }
    library_view_render();
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    ESP_LOGI(TAG, "打开曲库：generation=%lu 当前列表=%s page=%lu",
        static_cast<unsigned long>(media_catalog_v2_generation()),
        player_playlist_type_name(player_state_get_list_type()),
        static_cast<unsigned long>(g_state.page + 1U));
}

bool library_view_is_visible()
{
    return g_root != nullptr && !lv_obj_has_flag(g_root, LV_OBJ_FLAG_HIDDEN);
}
