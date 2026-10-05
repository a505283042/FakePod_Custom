# FakePod NAS Static Short-ID Alias V1 (DSM 5.2 / ext4)

R46.0.96 removes long UTF-8 paths from ESP32 playback without Python or Docker on the NAS.
The existing DSM static HTTP service serves hard links named by stable 64-bit IDs.

## 1. Build the catalog on Windows

```powershell
python tools\build_nas_catalog_v2.py "\\192.168.1.105\music" "\\192.168.1.105\web\music-index"
```

Besides the normal V2 catalog, the builder writes `fakepod_make_static_track_links.sh`.
It no longer writes `fakepod_track_map_v1.json`; no dynamic NAS process is required.

## 2. Build aliases once on DSM

The user's `music` and `web` shares are on the same ext4 Storage Space 1, so hard links do not
copy audio payload data. Enable SSH in DSM and run:

```sh
sh /volume1/web/music-index/fakepod_make_static_track_links.sh /volume1/music /volume1/web/track
```

The script builds a complete temporary sibling directory first. Only after every hard link succeeds
does it swap the new directory into `/volume1/web/track`. A missing source or cross-filesystem `ln`
failure leaves the previous working alias directory untouched.

## 3. Device config

On this DSM 5.2 deployment the catalog remains at the existing `/web/music-index` URL, while static aliases under `/volume1/web/track` are served at the HTTP-root `/track` URL.

```text
base_url=http://192.168.1.105:8080/web/music-index
track_url=http://192.168.1.105:8080/track
music_url=http://192.168.1.105:8080/music
username=
password=
```

Playback then uses a normal static file URL such as:

```text
http://192.168.1.105:8080/track/E4BC288E699130F0.flac
```

## 4. After future library changes

Re-run the Windows catalog builder, re-run the generated shell script over SSH, then trigger NAS
catalog sync on FakePod.
