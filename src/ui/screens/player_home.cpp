#include "player_home.h"

#include <stdio.h>
#include "esp_log.h"
#include "audio_service.h"
#include "font/font_manager.h"
#include "media_library.h"
#include "player_control.h"
#include "player_state.h"
#include "ui_common.h"

static const char *TAG = "首页";
static lv_obj_t *g_title = nullptr;
static lv_obj_t *g_track_info = nullptr;
static lv_obj_t *g_play_symbol = nullptr;
static lv_obj_t *g_progress = nullptr;
static uint32_t g_last_audio_state_revision = UINT32_MAX;

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

static void player_home_refresh_track(const AudioStateSnapshot *audio_snapshot)
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

    char suffix[96] = {};
    if (audio_snapshot != nullptr && audio_snapshot->track_index == index) {
        if (
            audio_snapshot->state == AudioPlaybackState::Error &&
            audio_snapshot->last_error == ESP_ERR_NOT_SUPPORTED
        ) {
            const MediaFormat format = player_state_get_format();
            if (format == MediaFormat::WAV || format == MediaFormat::FLAC || format == MediaFormat::MP3) {
                snprintf(suffix, sizeof(suffix), "  · 参数暂不支持");
            } else {
                snprintf(suffix, sizeof(suffix), "  · 解码器待接入");
            }
        } else if (audio_snapshot->state == AudioPlaybackState::Playing) {
            if (audio_snapshot->sample_rate_hz == 44100) {
                snprintf(suffix, sizeof(suffix), "  · 播放中 · 44.1k/%ubit",
                    static_cast<unsigned>(audio_snapshot->bits_per_sample));
            } else if (audio_snapshot->sample_rate_hz > 0) {
                snprintf(suffix, sizeof(suffix), "  · 播放中 · %luk/%ubit",
                    static_cast<unsigned long>(audio_snapshot->sample_rate_hz / 1000),
                    static_cast<unsigned>(audio_snapshot->bits_per_sample));
            } else {
                snprintf(suffix, sizeof(suffix), "  · 播放中");
            }
        } else if (audio_snapshot->state == AudioPlaybackState::Paused) {
            if (audio_snapshot->sample_rate_hz == 44100) {
                snprintf(suffix, sizeof(suffix), "  · 已暂停 · 44.1k/%ubit",
                    static_cast<unsigned>(audio_snapshot->bits_per_sample));
            } else if (audio_snapshot->sample_rate_hz > 0) {
                snprintf(suffix, sizeof(suffix), "  · 已暂停 · %luk/%ubit",
                    static_cast<unsigned long>(audio_snapshot->sample_rate_hz / 1000),
                    static_cast<unsigned>(audio_snapshot->bits_per_sample));
            } else {
                snprintf(suffix, sizeof(suffix), "  · 已暂停");
            }
        } else if (audio_snapshot->state == AudioPlaybackState::Finished) {
            snprintf(suffix, sizeof(suffix), "  · 播放结束");
        }
    }

    lv_label_set_text_fmt(
        g_track_info,
        "第 %u / %u 首  %s%s",
        static_cast<unsigned>(index + 1),
        static_cast<unsigned>(count),
        media_library_format_name(player_state_get_format()),
        suffix);
}

static void player_home_apply_audio_snapshot(const AudioStateSnapshot &snapshot)
{
    if (g_play_symbol != nullptr) {
        const bool show_pause = snapshot.state == AudioPlaybackState::Playing;
        lv_label_set_text(g_play_symbol, show_pause ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    if (g_progress != nullptr) {
        int32_t progress = 0;
        if (
            snapshot.track_index == player_state_get_index() &&
            snapshot.total_frames > 0
        ) {
            uint64_t percent = (snapshot.position_frames * 100ULL) / snapshot.total_frames;
            if (percent > 100) {
                percent = 100;
            }
            progress = static_cast<int32_t>(percent);
        }
        lv_bar_set_value(g_progress, progress, LV_ANIM_OFF);
    }

    player_home_refresh_track(&snapshot);
}

static void player_home_audio_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot)) {
        return;
    }
    if (snapshot.state_revision == g_last_audio_state_revision) {
        return;
    }
    g_last_audio_state_revision = snapshot.state_revision;
    player_home_apply_audio_snapshot(snapshot);
}

static void player_home_prev_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (player_control_previous()) {
        AudioStateSnapshot snapshot = {};
        audio_service_get_snapshot(&snapshot);
        player_home_apply_audio_snapshot(snapshot);
    }
}

static void player_home_next_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (player_control_next()) {
        AudioStateSnapshot snapshot = {};
        audio_service_get_snapshot(&snapshot);
        player_home_apply_audio_snapshot(snapshot);
    }
}

static void player_home_play_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (!player_control_toggle_play_pause()) {
        ESP_LOGW(TAG, "播放控制请求未能入队");
    }
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
    lv_obj_set_size(g_track_info, 420, 32);
    lv_obj_set_style_text_align(g_track_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_track_info, LV_ALIGN_TOP_MID, 0, 329);

    g_progress = lv_bar_create(screen);
    ui_common_lock_object(g_progress);
    lv_obj_set_size(g_progress, 330, 6);
    lv_obj_align(g_progress, LV_ALIGN_TOP_MID, 0, 365);
    lv_bar_set_range(g_progress, 0, 100);
    lv_bar_set_value(g_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_progress, lv_color_hex(0x282E38), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_progress, lv_color_hex(0xF2F3F5), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_COVER, LV_PART_INDICATOR);

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

    AudioStateSnapshot snapshot = {};
    audio_service_get_snapshot(&snapshot);
    player_home_apply_audio_snapshot(snapshot);
    g_last_audio_state_revision = snapshot.state_revision;
    lv_timer_create(player_home_audio_timer_cb, 100, nullptr);

    ESP_LOGI(TAG, "Stage 9.5.1 播放器首页已接入 WAV/FLAC/MP3 统一 PCM Core 与进度快照，当前歌曲=%u/%u",
        static_cast<unsigned>(media_library_get_count() > 0 ? player_state_get_index() + 1 : 0),
        static_cast<unsigned>(media_library_get_count()));
}
