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

// R46.0.70：NAS Catalog 与本地 V2 Catalog 使用完全相同的二进制格式，但独立缓存，
// 绝不覆盖当前 TF 曲库。NAS 端只需要通过 HTTP/WebDAV GET 暴露三个固定文件。
static constexpr const char *kNasCatalogConfig = "/sdcard/System/nas_catalog.conf";
static constexpr const char *kNasCatalogMeta = "/sdcard/System/library/nas_catalog.meta";
static constexpr const char *kNasCatalogMetaTemp = "/sdcard/System/library/nas_catalog.meta.tmp";
static constexpr const char *kNasCatalogMetaBackup = "/sdcard/System/library/nas_catalog.meta.bak";
static constexpr const char *kNasMusicIndexV2 = "/sdcard/System/library/nas_music_index_v2.bin";
static constexpr const char *kNasMusicIndexV2Temp = "/sdcard/System/library/nas_music_index_v2.bin.tmp";
static constexpr const char *kNasMusicIndexV2Backup = "/sdcard/System/library/nas_music_index_v2.bin.bak";
static constexpr const char *kNasMusicManifestV2 = "/sdcard/System/library/nas_music_manifest_v2.bin";
static constexpr const char *kNasMusicManifestV2Temp = "/sdcard/System/library/nas_music_manifest_v2.bin.tmp";
static constexpr const char *kNasMusicManifestV2Backup = "/sdcard/System/library/nas_music_manifest_v2.bin.bak";
}
