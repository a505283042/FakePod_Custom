#include "player_home.h"

#include <stdio.h>

#include "esp_log.h"
#include "font/font_manager.h"
#include "media_library.h"
#include "player_state.h"
#include "ui_common.h"

static const char *TAG = "首页";
static lv_obj_t *g_title = nullptr;
static lv_obj_t *g_track_info = nullptr;
static lv_obj_t *g_play_symbol = nullptr;
static bool g_fake_playing = false;

static lv_obj_t *player_home_create_label(
    lv_obj_t *parent,
    const char *text,
    lv_color_t color,
    const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    ui_common_lock_object(label);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font != nullptr ? font : lv_font_default(), 0);
    return label;
}

static lv_obj_t *player_home_create_round_button(lv_obj_t *parent, int32_t size, const char *symbol)
{
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x252B36), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);

    lv_obj_t *label = player_home_create_label(
        button, symbol, lv_color_hex(0xFFFFFF), lv_font_default());
    lv_obj_center(label);
    return button;
}

static void player_home_refresh_track()
{
    if (g_title == nullptr || g_track_info == nullptr) {
        return;
    }
    const size_t count = media_library_get_count();
    if (count == 0) {
        lv_label_set_text(g_title, "暂无歌曲");
        lv_label_set_text(g_track_info, "音乐库为空");
        return;
    }

    const size_t index = player_state_get_index();
    char title[512] = {};
    if (!media_library_copy_display_name(index, title, sizeof(title))) {
        snprintf(title, sizeof(title), "歌曲 %u", static_cast<unsigned>(index + 1));
    }
    lv_label_set_text(g_title, title);
    lv_label_set_text_fmt(
        g_track_info,
        "第 %u / %u 首  %s",
        static_cast<unsigned>(index + 1),
        static_cast<unsigned>(count),
        media_library_format_name(player_state_get_format()));
}

static void player_home_prev_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (player_state_previous()) {
        g_fake_playing = false;
        if (g_play_symbol != nullptr) {
            lv_label_set_text(g_play_symbol, LV_SYMBOL_PLAY);
        }
        player_home_refresh_track();
    }
}

static void player_home_next_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (player_state_next()) {
        g_fake_playing = false;
        if (g_play_symbol != nullptr) {
            lv_label_set_text(g_play_symbol, LV_SYMBOL_PLAY);
        }
        player_home_refresh_track();
    }
}

static void player_home_play_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (media_library_get_count() == 0) {
        ESP_LOGW(TAG, "音乐库为空，无法切换播放状态");
        return;
    }
    g_fake_playing = !g_fake_playing;
    if (g_play_symbol != nullptr) {
        lv_label_set_text(g_play_symbol, g_fake_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    ESP_LOGI(TAG, "%s：%s", g_fake_playing ? "模拟播放" : "模拟暂停", player_state_get_path());
}

void player_home_create(lv_obj_t *screen)
{
    if (screen == nullptr) {
        return;
    }

    ui_common_lock_object(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0E1117), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *brand = player_home_create_label(
        screen, "FakePod", lv_color_hex(0xF4F5F7), lv_font_default());
    lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 30, 26);

    lv_obj_t *sd = player_home_create_label(
        screen, LV_SYMBOL_SD_CARD, lv_color_hex(0xAAB2BF), lv_font_default());
    lv_obj_align(sd, LV_ALIGN_TOP_RIGHT, -32, 26);

    lv_obj_t *cover = lv_obj_create(screen);
    ui_common_lock_object(cover);
    lv_obj_set_size(cover, 220, 220);
    lv_obj_align(cover, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_style_radius(cover, 26, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x1B2029), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_shadow_width(cover, 0, 0);

    lv_obj_t *cover_icon = player_home_create_label(
        cover, LV_SYMBOL_AUDIO, lv_color_hex(0x6F7A89), lv_font_default());
    lv_obj_center(cover_icon);

    g_title = player_home_create_label(
        screen, "", lv_color_hex(0xFFFFFF), font_manager_get_ui_font());
    lv_label_set_long_mode(g_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_title, 400, 32);
    lv_obj_set_style_text_align(g_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 294);

    g_track_info = player_home_create_label(
        screen, "", lv_color_hex(0x7E8795), font_manager_get_ui_font());
    lv_label_set_long_mode(g_track_info, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_track_info, 400, 32);
    lv_obj_set_style_text_align(g_track_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_track_info, LV_ALIGN_TOP_MID, 0, 329);

    lv_obj_t *progress = lv_bar_create(screen);
    ui_common_lock_object(progress);
    lv_obj_set_size(progress, 330, 6);
    lv_obj_align(progress, LV_ALIGN_TOP_MID, 0, 365);
    lv_bar_set_range(progress, 0, 100);
    lv_bar_set_value(progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress, lv_color_hex(0x282E38), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress, lv_color_hex(0xF2F3F5), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_t *prev = player_home_create_round_button(screen, 58, LV_SYMBOL_PREV);
    lv_obj_align(prev, LV_ALIGN_BOTTOM_MID, -92, -24);
    lv_obj_add_event_cb(prev, player_home_prev_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *play = player_home_create_round_button(screen, 74, LV_SYMBOL_PLAY);
    lv_obj_align(play, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(play, lv_color_hex(0xF2F3F5), 0);
    g_play_symbol = lv_obj_get_child(play, 0);
    if (g_play_symbol != nullptr) {
        lv_obj_set_style_text_color(g_play_symbol, lv_color_hex(0x11151B), 0);
    }
    lv_obj_add_event_cb(play, player_home_play_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *next = player_home_create_round_button(screen, 58, LV_SYMBOL_NEXT);
    lv_obj_align(next, LV_ALIGN_BOTTOM_MID, 92, -24);
    lv_obj_add_event_cb(next, player_home_next_cb, LV_EVENT_CLICKED, nullptr);

    player_home_refresh_track();
    ESP_LOGI(TAG, "Stage 7.2 播放器首页创建完成，当前歌曲=%u/%u",
        static_cast<unsigned>(media_library_get_count() > 0 ? player_state_get_index() + 1 : 0),
        static_cast<unsigned>(media_library_get_count()));
}
