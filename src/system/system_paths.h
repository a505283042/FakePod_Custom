#pragma once

// FakePod 运行期生成文件统一放在 /sdcard/System。
// 曲库持久化文件统一集中到 /System/library；V1 路径仅保留作一次性迁移源。
namespace SystemPaths
{
static constexpr const char *kSystemDirectory = "/sdcard/System";
static constexpr const char *kNoCoverArtwork = "/sdcard/System/no_cover_artwork.jpg";
static constexpr const char *kNoCoverCassette = "/sdcard/System/no_cover_cassette.jpg";
static constexpr const char *kLibraryDirectory = "/sdcard/System/library";
static constexpr const char *kMusicIndex = "/sdcard/System/music_index_v1.bin";
static constexpr const char *kMusicIndexTemp = "/sdcard/System/music_index_v1.bin.tmp";
static constexpr const char *kMusicIndexBackup = "/sdcard/System/music_index_v1.bin.bak";
static constexpr const char *kMusicManifest = "/sdcard/System/music_manifest_v1.bin";
static constexpr const char *kMusicManifestTemp = "/sdcard/System/music_manifest_v1.bin.tmp";
static constexpr const char *kMusicManifestBackup = "/sdcard/System/music_manifest_v1.bin.bak";

static constexpr const char *kMusicIndexV2 = "/sdcard/System/library/music_index_v2.bin";
static constexpr const char *kMusicIndexV2Temp = "/sdcard/System/library/music_index_v2.bin.tmp";
static constexpr const char *kMusicIndexV2Backup = "/sdcard/System/library/music_index_v2.bin.bak";
static constexpr const char *kMusicManifestV2 = "/sdcard/System/library/music_manifest_v2.bin";
static constexpr const char *kMusicManifestV2Temp = "/sdcard/System/library/music_manifest_v2.bin.tmp";
static constexpr const char *kMusicManifestV2Backup = "/sdcard/System/library/music_manifest_v2.bin.bak";
static constexpr const char *kMusicQuickStamp = "/sdcard/System/library/music_quick_stamp_v1.bin";
}
