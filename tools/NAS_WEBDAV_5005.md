# R46.0.117 — Synology WebDAV 5005 (NAS transport only)

Baseline: R46.0.116. The V2 catalog, metadata, queue, UI, Wi-Fi/BLE policy, MP3/FLAC
rings and decoding remain unchanged. No new server on the NAS; this is WebDAV GET,
not DLNA. The old HTTP 8080 setup stays valid as a rollback.

## Already verified on DSM 5.2 / 192.168.1.105

- `PROPFIND /麦田广告/Music/`: 207 and UTF-8 filenames.
- GET `/麦田广告/Music/我喜欢/朱雅 - 前度.flac`: 206 range 0–1023/24301502.
- GET `/web/music-index/music_index_v2.bin`: 200, 982194 bytes.
- GET `/web/music-index/music_manifest_v2.bin`: 200, 101480 bytes.
- These are Windows/curl checks; **ESP32 has not been compiled or tested**.

## Deployment steps (no NAS file relocation)

1. Leave the 8080 service, hardlinks and existing index in place until ESP32 playback works.
2. On DSM, enable WebDAV HTTP on LAN port 5005. Create a dedicated NAS user with
   read-only permission to shares `web` and `麦田广告`, but no write/admin access.
3. On TF card, BACK UP `/sdcard/System/nas_catalog.conf`, then replace only its
   content with `tools/nas_catalog_webdav.conf.example` using the new account.
   The `track_url=` line must be empty. **Do not keep the old 8080 URL there.**
   `music_url` contains the percent-encoded `麦田广告` share name and `/Music`.
4. Do not rebuild V2 solely to change the protocol; V2 stores paths relative to
   the directory selected as `music_root` when index was generated. That music
   root MUST be the WebDAV music_url target, with the same child path names. If
   previously built from a different directory, first reconcile it or rebuild
   V2 from the exact `麦田广告/Music` directory and sync.
5. Trigger existing NAS index sync (200 meta, index and manifest) and play MP3/FLAC.
   Test long Chinese paths, list/artist/album/decade, queue next/previous and
   pause/resume. Log peak INTERNAL free, stack HWM, HTTP TX bytes, and buffering.
6. When tested successfully, disable 8080. Delete legacy `/web/track` aliases
   only after ensuring no other programs use them; never remove actual music files.

## Important limits

- The URL builder already percent-encodes the UTF-8 V2 relative path. The
  configured WebDAV music_url prefix must be encoded once, as shown.
- The HTTP audio TX buffer remains 1024B for short URLs; long WebDAV paths
  request only enough extra temporary INTERNAL RAM to fit (up to 4096B). Paths
  above that bound fail instead of overflowing. Measure on the real device.
- DSM 5.2 HTTP Basic sends credentials unencrypted. Keep 5005 LAN-only; a
  later 5006 HTTPS variant requires separate TLS and RAM compatibility checks.
- Plaintext password remains in existing `nas_catalog.conf` on the SD card,
  not committed in this repository. Do not paste real credentials into chats.
- Updating V2 remains the same Python builder workflow. The builder may still
  generate an unused short-ID script; it is not needed for WebDAV playback.
- This patch does NOT add directory crawling or modify V2's binary format.

## Rollback

Restore the backed-up `nas_catalog.conf` with original HTTP 8080 base_url,
track_url and music_url, keep aliases on NAS, and reconnect Wi-Fi. No V2 or
player-state migration is required.
