#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "media_catalog_v2.h"
#include "media_groups_v2.h"
#include "player_playlist.h"

// R46.0.73: NAS no longer owns a second UI. This module only exposes the cached
// NAS V2 Catalog as a read-only data source for the existing library_view.
enum class NasLibraryOpenStage : uint8_t
{
    ReadingCatalog = 0,
    BuildingGroups,
    PreparingScope,
};

using NasLibraryOpenStageCallback = void (*)(NasLibraryOpenStage stage, void *context);

// R46.0.74：后台分步打开。Catalog 走 cooperative SD 读取，分组/范围构建在低优先级 Worker 执行。
// callback 只在 Worker 上下文触发，调用方不得直接在其中操作 LVGL。
esp_err_t nas_library_source_open(
    PlayerFolderScope initial_scope,
    NasLibraryOpenStageCallback callback = nullptr,
    void *callback_context = nullptr);
void nas_library_source_close();
bool nas_library_source_ready();
uint32_t nas_library_source_generation();
uint32_t nas_library_source_track_count();
size_t nas_library_source_psram_bytes();

bool nas_library_source_get_track_view(uint32_t track_index, MediaTrackViewV2 *out_view);
size_t nas_library_source_artist_count();
size_t nas_library_source_album_count();
size_t nas_library_source_decade_count();
bool nas_library_source_get_artist(size_t group_index, MediaArtistGroupViewV2 *out_view);
bool nas_library_source_get_album(size_t group_index, MediaAlbumGroupViewV2 *out_view);
bool nas_library_source_get_decade(size_t group_index, MediaDecadeGroupViewV2 *out_view);

PlayerFolderScope nas_library_source_folder_scope();
bool nas_library_source_set_folder_scope(PlayerFolderScope scope);
uint32_t nas_library_source_folder_context_id();
uint32_t nas_library_source_scoped_track_count();
bool nas_library_source_scoped_track_at(uint32_t position, uint32_t *out_track_index);

size_t nas_library_source_folder_option_count(PlayerFolderScope scope, const char *parent_path);
bool nas_library_source_copy_folder_option_at(
    PlayerFolderScope scope,
    const char *parent_path,
    size_t position,
    char *out_path,
    size_t out_path_size,
    uint32_t *out_track_count);
bool nas_library_source_set_folder_selection(PlayerFolderScope scope, const char *path);
bool nas_library_source_copy_folder_selection(PlayerFolderScope scope, char *out_path, size_t out_path_size);
bool nas_library_source_folder_selection_available(PlayerFolderScope scope, const char *path, uint32_t *out_track_count);
