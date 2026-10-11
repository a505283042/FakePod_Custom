#!/usr/bin/env python3
"""Build FakePod NAS Catalog files without ESP32-side directory scanning.

Outputs are byte-compatible with FakePod's V2 catalog reader:
  - music_index_v2.bin    (FPCATV2 version 6)
  - music_manifest_v2.bin (FPMNFV2 version 2)
  - fakepod_catalog.meta  (tiny revision/size/CRC descriptor)
  - fakepod_make_static_track_links.sh (DSM/ext4 hard-link builder for short-ID aliases)

R46.0.72 enriches the NAS-side catalog with lightweight media tags using only
Python's standard library. The ESP32 still only downloads/validates the finished
index and never scans the NAS music tree itself.
"""

from __future__ import annotations

import argparse
import json
import os
import uuid
import re
import struct
import zlib
from dataclasses import dataclass, asdict, fields
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

from nas_artwork_locator_v2 import scan_artwork, find_directory_cover

CATALOG_VERSION = 6
MANIFEST_VERSION = 2
SIGNATURE_MODE_FAST = 1
INVALID_ID = 0xFFFFFFFF
SECTION_COUNT = 7

SEC_STR_POOL = 1
SEC_ARTISTS = 2
SEC_ALBUMS = 3
SEC_TRACK_ARTIST_REFS = 4
SEC_LYRICS_REFS = 5
SEC_ARTWORK_REFS = 6
SEC_TRACKS = 7

META_HAS_TRACK_NUMBER = 1 << 0
META_HAS_TRACK_TOTAL = 1 << 1
META_HAS_DISC_NUMBER = 1 << 2
META_HAS_DISC_TOTAL = 1 << 3
META_HAS_RELEASE_YEAR = 1 << 4
META_HAS_ORIGINAL_YEAR = 1 << 5
META_SCANNED = 1 << 6
META_HAS_TITLE_TAG = 1 << 7
META_HAS_ARTIST_TAG = 1 << 8
META_HAS_ALBUM_TAG = 1 << 9
META_HAS_ALBUM_ARTIST_TAG = 1 << 10

FORMAT_MAP = {
    ".mp3": 1,
    ".flac": 2,
    ".wav": 3,
    ".opus": 6,
}

CATALOG_HEADER = struct.Struct("<8sHHHHIIIIIIIII")
SECTION = struct.Struct("<IIIIHHI")
ARTIST = struct.Struct("<II")
ALBUM = struct.Struct("<IIIIIHH")
ARTIST_REF = struct.Struct("<II")
ARTWORK = struct.Struct("<QIIqIHHBBBB")
LYRICS = struct.Struct("<QIIIIBBBB")
TRACK = struct.Struct("<IIIIHHIHHIIIHHHHHHBBBBIQIIIQQQQIIHH")
MANIFEST_HEADER = struct.Struct("<8sHHHHIIII")
MANIFEST_ROW = struct.Struct("<IB3xQq")

assert CATALOG_HEADER.size == 52
assert SECTION.size == 24
assert ARTIST.size == 8
assert ALBUM.size == 24
assert ARTIST_REF.size == 8
assert ARTWORK.size == 36
assert LYRICS.size == 28
assert TRACK.size == 124
assert MANIFEST_HEADER.size == 32
assert MANIFEST_ROW.size == 24

YEAR_RE = re.compile(r"(?<!\d)(18\d{2}|19\d{2}|20\d{2}|21\d{2})(?!\d)")
NUMBER_RE = re.compile(r"^\s*(\d{1,5})(?:\s*/\s*(\d{1,5}))?")


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def stable_track_id(relative_path: str) -> int:
    """64-bit FNV-1a over the exact UTF-8 catalog path.

    Firmware uses the same tiny hash, so the V2 catalog row layout does not grow.
    A path rename intentionally creates a new ID; unchanged paths keep the same ID
    across rescans and sorting changes.
    """
    value = 0xCBF29CE484222325
    for byte in relative_path.encode("utf-8"):
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def shell_single_quote(value: str) -> str:
    """POSIX shell single-quote escaping; BusyBox /bin/sh compatible."""
    return "'" + value.replace("'", "'\"'\"'") + "'"


def build_static_alias_script(tracks: List["TrackMeta"]) -> bytes:
    """Build an atomic hard-link set for DSM 5.2/ext4."""
    lines = [
        "#!/bin/sh",
        "set -eu",
        'MUSIC_ROOT="${1:-/volume1/music}"',
        'TRACK_ROOT="${2:-$MUSIC_ROOT/_FakePod/track}"',
        'PARENT="$(dirname "$TRACK_ROOT")"',
        'NAME="$(basename "$TRACK_ROOT")"',
        'TMP="$PARENT/.${NAME}.new.$$"',
        'OLD="$PARENT/.${NAME}.old.$$"',
        'cleanup() { rm -rf "$TMP"; }',
        "trap cleanup EXIT INT TERM",
        'rm -rf "$TMP" "$OLD"',
        'mkdir -p "$TMP"',
        'echo "FakePod static short-ID aliases: building ${TRACK_ROOT}"',
    ]
    for track in tracks:
        suffix = Path(track.path).suffix
        if suffix.lower() not in (".mp3", ".flac"):
            continue
        relative = track.path.lstrip("/")
        src_tail = shell_single_quote(relative)
        alias_name = shell_single_quote(f"{track.track_id:016X}{suffix}")
        lines.append(f'ln "$MUSIC_ROOT"/{src_tail} "$TMP"/{alias_name}')
    lines.extend([
        # Existing aliases might include manual compatibility names for old
        # V2 catalog paths. Preserve them when rebuilding rather than
        # discarding them during the atomic directory swap.
        'if [ -d "$TRACK_ROOT" ]; then',
        '  for PREVIOUS_ALIAS in "$TRACK_ROOT"/*; do',
        '    [ -f "$PREVIOUS_ALIAS" ] || continue',
        '    ALIAS_NAME="${PREVIOUS_ALIAS##*/}"',
        '    if [ ! -e "$TMP/$ALIAS_NAME" ]; then',
        '      ln "$PREVIOUS_ALIAS" "$TMP/$ALIAS_NAME"',
        '    fi',
        '  done',
        'fi',
        'if [ -d "$TRACK_ROOT" ]; then mv "$TRACK_ROOT" "$OLD"; fi',
        'if ! mv "$TMP" "$TRACK_ROOT"; then',
        '  if [ -d "$OLD" ]; then mv "$OLD" "$TRACK_ROOT"; fi',
        '  exit 1',
        'fi',
        'rm -rf "$OLD"',
        "trap - EXIT INT TERM",
        'COUNT="$(find "$TRACK_ROOT" -type f | wc -l | tr -d " ")"',
        'echo "FakePod static short-ID aliases ready: files=${COUNT} path=${TRACK_ROOT}"',
        "",
    ])
    return "\n".join(lines).encode("utf-8")


def ascii_fold_key(text: str) -> bytes:
    raw = text.encode("utf-8")
    return bytes((b + 32) if 65 <= b <= 90 else b for b in raw)


def clean_text(value: Optional[str]) -> str:
    if not value:
        return ""
    return " ".join(value.replace("\x00", " ").split()).strip()


def parse_year(value: str) -> int:
    match = YEAR_RE.search(value or "")
    return int(match.group(1)) if match else 0


def parse_number_pair(value: str) -> Tuple[int, int]:
    match = NUMBER_RE.match(value or "")
    if not match:
        return 0, 0
    first = int(match.group(1))
    total = int(match.group(2)) if match.group(2) else 0
    return min(first, 0xFFFF), min(total, 0xFFFF)


def syncsafe32(raw: bytes) -> int:
    if len(raw) != 4:
        return 0
    return ((raw[0] & 0x7F) << 21) | ((raw[1] & 0x7F) << 14) | ((raw[2] & 0x7F) << 7) | (raw[3] & 0x7F)


def deunsync(data: bytes) -> bytes:
    return data.replace(b"\xff\x00", b"\xff")


def decode_id3_text(payload: bytes) -> str:
    if not payload:
        return ""
    enc = payload[0]
    data = payload[1:]
    try:
        if enc == 0:
            text = data.decode("latin-1", errors="replace")
        elif enc == 1:
            text = data.decode("utf-16", errors="replace")
        elif enc == 2:
            text = data.decode("utf-16-be", errors="replace")
        else:
            text = data.decode("utf-8", errors="replace")
    except (LookupError, UnicodeError):
        return ""
    return clean_text(text)


def parse_id3_blob(blob: bytes) -> Dict[str, List[str]]:
    tags: Dict[str, List[str]] = {}
    if len(blob) < 10 or blob[:3] != b"ID3":
        return tags
    version = blob[3]
    if version not in (3, 4):
        return tags
    flags = blob[5]
    size = syncsafe32(blob[6:10])
    data = blob[10:10 + size]
    if flags & 0x80:
        data = deunsync(data)
    pos = 0
    if flags & 0x40 and len(data) >= 4:
        if version == 3:
            ext_size = int.from_bytes(data[:4], "big", signed=False)
            pos = min(len(data), 4 + ext_size)
        else:
            ext_size = syncsafe32(data[:4])
            pos = min(len(data), ext_size)
    wanted = {"TIT2", "TPE1", "TALB", "TPE2", "TDRC", "TYER", "TDOR", "TORY", "TRCK", "TPOS"}
    while pos + 10 <= len(data):
        frame_id_raw = data[pos:pos + 4]
        if frame_id_raw == b"\x00\x00\x00\x00":
            break
        try:
            frame_id = frame_id_raw.decode("ascii")
        except UnicodeDecodeError:
            break
        if not frame_id.isalnum():
            break
        raw_size = data[pos + 4:pos + 8]
        frame_size = syncsafe32(raw_size) if version == 4 else int.from_bytes(raw_size, "big", signed=False)
        pos += 10
        if frame_size <= 0 or pos + frame_size > len(data):
            break
        payload = data[pos:pos + frame_size]
        pos += frame_size
        if frame_id in wanted:
            text = decode_id3_text(payload)
            if text:
                tags.setdefault(frame_id, []).append(text)
    return tags


def read_mp3_tags(path: Path) -> Dict[str, List[str]]:
    try:
        with path.open("rb") as f:
            header = f.read(10)
            if len(header) < 10 or header[:3] != b"ID3":
                return {}
            size = syncsafe32(header[6:10])
            if size > 8 * 1024 * 1024:
                return {}
            return parse_id3_blob(header + f.read(size))
    except OSError:
        return {}


def parse_vorbis_comment(data: bytes) -> Dict[str, List[str]]:
    tags: Dict[str, List[str]] = {}
    if len(data) < 8:
        return tags
    pos = 0
    vendor_len = int.from_bytes(data[pos:pos + 4], "little", signed=False)
    pos += 4
    if vendor_len > len(data) - pos:
        return tags
    pos += vendor_len
    if pos + 4 > len(data):
        return tags
    count = int.from_bytes(data[pos:pos + 4], "little", signed=False)
    pos += 4
    if count > 100000:
        return tags
    for _ in range(count):
        if pos + 4 > len(data):
            break
        length = int.from_bytes(data[pos:pos + 4], "little", signed=False)
        pos += 4
        if length > len(data) - pos:
            break
        raw = data[pos:pos + length]
        pos += length
        try:
            text = raw.decode("utf-8", errors="replace")
        except UnicodeError:
            continue
        if "=" not in text:
            continue
        key, value = text.split("=", 1)
        key = key.upper().strip()
        value = clean_text(value)
        if key and value:
            tags.setdefault(key, []).append(value)
    return tags


def read_flac_tags(path: Path) -> Dict[str, List[str]]:
    try:
        with path.open("rb") as f:
            if f.read(4) != b"fLaC":
                return {}
            for _ in range(128):
                header = f.read(4)
                if len(header) != 4:
                    break
                last = (header[0] & 0x80) != 0
                block_type = header[0] & 0x7F
                length = int.from_bytes(header[1:4], "big", signed=False)
                if block_type == 4:
                    if length > 4 * 1024 * 1024:
                        return {}
                    return parse_vorbis_comment(f.read(length))
                f.seek(length, os.SEEK_CUR)
                if last:
                    break
    except OSError:
        pass
    return {}


def read_ogg_packets(path: Path, byte_limit: int = 2 * 1024 * 1024) -> Iterable[bytes]:
    packet = bytearray()
    consumed = 0
    try:
        with path.open("rb") as f:
            while consumed < byte_limit:
                header = f.read(27)
                consumed += len(header)
                if len(header) != 27 or header[:4] != b"OggS":
                    return
                seg_count = header[26]
                lacing = f.read(seg_count)
                consumed += len(lacing)
                if len(lacing) != seg_count:
                    return
                payload_size = sum(lacing)
                payload = f.read(payload_size)
                consumed += len(payload)
                if len(payload) != payload_size:
                    return
                pos = 0
                for seg in lacing:
                    packet.extend(payload[pos:pos + seg])
                    pos += seg
                    if seg < 255:
                        yield bytes(packet)
                        packet.clear()
    except OSError:
        return


def read_opus_tags(path: Path) -> Dict[str, List[str]]:
    for packet in read_ogg_packets(path):
        if packet.startswith(b"OpusTags"):
            return parse_vorbis_comment(packet[8:])
        if len(packet) > 1024 * 1024:
            break
    return {}


def read_wav_tags(path: Path) -> Dict[str, List[str]]:
    tags: Dict[str, List[str]] = {}
    info_map = {
        b"INAM": "TITLE",
        b"IART": "ARTIST",
        b"IPRD": "ALBUM",
        b"ICRD": "DATE",
        b"ITRK": "TRACKNUMBER",
    }
    try:
        with path.open("rb") as f:
            head = f.read(12)
            if len(head) != 12 or head[:4] not in (b"RIFF", b"RF64") or head[8:12] != b"WAVE":
                return tags
            for _ in range(4096):
                chunk_head = f.read(8)
                if len(chunk_head) != 8:
                    break
                chunk_id = chunk_head[:4]
                size = int.from_bytes(chunk_head[4:8], "little", signed=False)
                if size > 64 * 1024 * 1024:
                    break
                if chunk_id == b"LIST" and size >= 4:
                    list_type = f.read(4)
                    remain = size - 4
                    if list_type == b"INFO" and remain <= 2 * 1024 * 1024:
                        data = f.read(remain)
                        pos = 0
                        while pos + 8 <= len(data):
                            key = data[pos:pos + 4]
                            n = int.from_bytes(data[pos + 4:pos + 8], "little", signed=False)
                            pos += 8
                            raw = data[pos:pos + n]
                            pos += n + (n & 1)
                            mapped = info_map.get(key)
                            if mapped and raw:
                                value = clean_text(raw.rstrip(b"\x00").decode("utf-8", errors="replace"))
                                if value:
                                    tags.setdefault(mapped, []).append(value)
                    else:
                        f.seek(remain, os.SEEK_CUR)
                elif chunk_id.lower() == b"id3 ":
                    if size <= 4 * 1024 * 1024:
                        return parse_id3_blob(f.read(size))
                    f.seek(size, os.SEEK_CUR)
                else:
                    f.seek(size, os.SEEK_CUR)
                if size & 1:
                    f.seek(1, os.SEEK_CUR)
    except OSError:
        pass
    return tags


def first(tags: Dict[str, List[str]], *keys: str) -> str:
    for key in keys:
        values = tags.get(key.upper()) or tags.get(key)
        if values:
            value = clean_text(values[0])
            if value:
                return value
    return ""


def joined(tags: Dict[str, List[str]], *keys: str) -> str:
    for key in keys:
        values = tags.get(key.upper()) or tags.get(key)
        if values:
            cleaned = [clean_text(v) for v in values if clean_text(v)]
            if cleaned:
                return " / ".join(dict.fromkeys(cleaned))
    return ""


@dataclass
class TrackMeta:
    path: str
    track_id: int
    title: str
    artist: str
    album: str
    album_artist: str
    fmt: int
    size: int
    mtime: int
    track_number: int = 0
    track_total: int = 0
    disc_number: int = 0
    disc_total: int = 0
    release_year: int = 0
    original_year: int = 0
    metadata_flags: int = META_SCANNED
    # NAS artwork uses the existing V2 artwork-ref section; none of these fields
    # affect the 124-byte track row, playback URL, or stable short ID.
    artwork_scanned: bool = False
    artwork_source: int = 0
    artwork_format: int = 0
    artwork_offset: int = 0
    artwork_size: int = 0
    artwork_flags: int = 0
    artwork_width: int = 0
    artwork_height: int = 0
    artwork_picture_type: int = 0
    artwork_path: str = ""
    artwork_mtime: int = 0
    # Same-name external synchronized .lrc; kept as a V2 LyricsRef locator.
    lyrics_path: str = ""


def tags_for_file(path: Path, fmt: int) -> Dict[str, List[str]]:
    if fmt == FORMAT_MAP[".mp3"]:
        raw = read_mp3_tags(path)
        return {
            "TITLE": raw.get("TIT2", []),
            "ARTIST": raw.get("TPE1", []),
            "ALBUM": raw.get("TALB", []),
            "ALBUMARTIST": raw.get("TPE2", []),
            "DATE": raw.get("TDRC", []) or raw.get("TYER", []),
            "ORIGINALDATE": raw.get("TDOR", []) or raw.get("TORY", []),
            "TRACKNUMBER": raw.get("TRCK", []),
            "DISCNUMBER": raw.get("TPOS", []),
        }
    if fmt == FORMAT_MAP[".flac"]:
        return read_flac_tags(path)
    if fmt == FORMAT_MAP[".opus"]:
        return read_opus_tags(path)
    if fmt == FORMAT_MAP[".wav"]:
        return read_wav_tags(path)
    return {}


def build_track_meta(path: Path, root: Path, fmt: int) -> TrackMeta:
    rel = "/" + path.relative_to(root).as_posix()
    stat = path.stat()
    tags = tags_for_file(path, fmt)

    title_tag = first(tags, "TITLE")
    artist_tag = joined(tags, "ARTIST")
    album_tag = first(tags, "ALBUM")
    album_artist_tag = joined(tags, "ALBUMARTIST", "ALBUM ARTIST")

    fallback_title = path.stem
    fallback_artist = ""
    if " - " in path.stem:
        possible_artist, possible_title = path.stem.split(" - ", 1)
        if possible_artist.strip() and possible_title.strip():
            fallback_artist = possible_artist.strip()
            fallback_title = possible_title.strip()

    title = title_tag or fallback_title
    artist = artist_tag or fallback_artist
    album = album_tag
    album_artist = album_artist_tag or artist

    track_number, track_total = parse_number_pair(first(tags, "TRACKNUMBER"))
    if track_total == 0:
        total_value, _ = parse_number_pair(first(tags, "TRACKTOTAL", "TOTALTRACKS"))
        track_total = total_value
    if track_total and track_number > track_total:
        track_total = 0
    disc_number, disc_total = parse_number_pair(first(tags, "DISCNUMBER"))
    if disc_total == 0:
        total_value, _ = parse_number_pair(first(tags, "DISCTOTAL", "TOTALDISCS"))
        disc_total = total_value
    if disc_total and disc_number > disc_total:
        disc_total = 0
    release_year = parse_year(first(tags, "DATE", "YEAR"))
    original_year = parse_year(first(tags, "ORIGINALDATE", "ORIGINALYEAR"))

    flags = META_SCANNED
    if title_tag:
        flags |= META_HAS_TITLE_TAG
    if artist_tag:
        flags |= META_HAS_ARTIST_TAG
    if album_tag:
        flags |= META_HAS_ALBUM_TAG
    if album_tag and album_artist_tag:
        flags |= META_HAS_ALBUM_ARTIST_TAG
    if track_number:
        flags |= META_HAS_TRACK_NUMBER
    if track_total:
        flags |= META_HAS_TRACK_TOTAL
    if disc_number:
        flags |= META_HAS_DISC_NUMBER
    if disc_total:
        flags |= META_HAS_DISC_TOTAL
    if release_year:
        flags |= META_HAS_RELEASE_YEAR
    if original_year:
        flags |= META_HAS_ORIGINAL_YEAR

    return TrackMeta(
        path=rel,
        track_id=stable_track_id(rel),
        title=clean_text(title),
        artist=clean_text(artist),
        album=clean_text(album),
        album_artist=clean_text(album_artist),
        fmt=fmt,
        size=stat.st_size,
        mtime=int(stat.st_mtime),
        track_number=track_number,
        track_total=track_total,
        disc_number=disc_number,
        disc_total=disc_total,
        release_year=release_year,
        original_year=original_year,
        metadata_flags=flags,
    )


CACHE_NAME = "fakepod_scan_cache_v1.json"
REPORT_NAME = "fakepod_change_report_v1.json"
INCREMENTAL_SCRIPT_NAME = "fakepod_incremental_update.sh"
CACHE_VERSION = 1


def cache_fingerprint(root: Path) -> str:
    # The root itself is not stored in the index; binds the cache to this SMB share.
    return str(root).replace("\\", "/").rstrip("/").casefold()


def read_scan_cache(path: Optional[Path], root: Path) -> Optional[dict]:
    if path is None or not path.exists():
        return None
    if path.stat().st_size > 24 * 1024 * 1024:
        raise ValueError("扫描缓存文件异常大，已停止，请先备份并检查")
    obj = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(obj, dict) or obj.get("version") != CACHE_VERSION:
        raise ValueError("扫描缓存版本不匹配，请备份后使用 --force-full 重建缓存")
    if obj.get("root") != cache_fingerprint(root):
        raise ValueError("扫描缓存对应另一音乐根目录，请重新选择 Music 或使用 --force-full")
    if not isinstance(obj.get("entries"), dict) or len(obj["entries"]) > 100000:
        raise ValueError("扫描缓存条目异常，已停止以免错误删除短 ID")
    return obj


def track_from_cache(path: str, item: dict) -> TrackMeta:
    allowed = {f.name for f in fields(TrackMeta)}
    row = item.get("track")
    # Compatible with old T3.3 caches that predate artwork locators.
    legacy = {f.name for f in fields(TrackMeta)} - {
        "artwork_scanned", "artwork_source", "artwork_format", "artwork_offset",
        "artwork_size", "artwork_flags", "artwork_width", "artwork_height",
        "artwork_picture_type", "artwork_path", "artwork_mtime", "lyrics_path",
    }
    pre_lyrics = allowed - {"lyrics_path"}
    if not isinstance(row, dict) or set(row) not in (allowed, legacy, pre_lyrics):
        raise ValueError(f"损坏的扫描缓存：{path}")
    result = TrackMeta(**row)
    if result.path != path or result.track_id != stable_track_id(path):
        raise ValueError(f"扫描缓存曲目不匹配：{path}")
    return result


def scan_incremental(root: Path, previous: Optional[dict]):
    """Stat all files; only parse tags for changed/new ones; emit a full V2 snapshot."""
    paths: List[Tuple[str, Path, int, os.stat_result]] = []
    visited = 0
    def on_walk_error(error: OSError) -> None:
        raise error
    for dirpath, dirnames, filenames in os.walk(root, onerror=on_walk_error):
        visited += 1
        if Path(dirpath) == root:
            dirnames[:] = [name for name in dirnames if name != "_FakePod"]
        for name in filenames:
            path = Path(dirpath) / name
            fmt = FORMAT_MAP.get(path.suffix.lower())
            if fmt is None:
                continue
            if not path.is_file():
                continue
            st = path.stat()
            relative = "/" + path.relative_to(root).as_posix()
            paths.append((relative, path, fmt, st))
        if visited % 30 == 0:
            print(f"FAKEPOD_PROGRESS DISCOVER {visited} {len(paths)}", flush=True)
    paths.sort(key=lambda item: ascii_fold_key(item[0]))
    print(f"FAKEPOD_PROGRESS FOUND {len(paths)} {len(paths)}", flush=True)
    if len(paths) > 100000:
        raise ValueError(f"too many tracks for FakePod V2: {len(paths)} > 100000")
    old = previous["entries"] if previous else {}
    rows = []
    folder_art_cache = {}
    artwork_scanned = 0
    new_entries = {}
    added, modified, unchanged = [], [], []
    previous_key = None
    seen_ids: Dict[int, str] = {}
    for n, (relative, path, fmt, st) in enumerate(paths, 1):
        key = ascii_fold_key(relative)
        if key == previous_key:
            raise ValueError(f"case-insensitive duplicate path is not valid in FakePod V2: {relative}")
        previous_key = key
        tid = stable_track_id(relative)
        if tid in seen_ids and seen_ids[tid] != relative:
            raise ValueError(f"stable track-id collision: {tid:016X} {seen_ids[tid]!r} {relative!r}")
        seen_ids[tid] = relative
        entry = old.get(relative)
        if (entry and isinstance(entry, dict) and
                entry.get("size") == st.st_size and entry.get("mtime_ns") == st.st_mtime_ns):
            row = track_from_cache(relative, entry)
            if row.fmt != fmt:
                raise ValueError(f"扫描缓存格式不一致：{relative}")
            # When only folder.jpg changes, the song's size/mtime does not.
            # Check the directory once per scan and invalidate external locators.
            if row.artwork_scanned and row.artwork_source in (0, 3):
                directory = path.parent
                if directory not in folder_art_cache:
                    folder_art_cache[directory] = find_directory_cover(directory, root)
                current_folder_art = folder_art_cache[directory]
                current = (row.artwork_source, row.artwork_format, row.artwork_offset,
                           row.artwork_size, row.artwork_flags, row.artwork_width,
                           row.artwork_height, row.artwork_picture_type,
                           row.artwork_path, row.artwork_mtime)
                if row.artwork_source == 3 and current_folder_art != current:
                    row.artwork_scanned = False
                if row.artwork_source == 0 and current_folder_art is not None:
                    row.artwork_scanned = False
            unchanged.append(relative)
        else:
            row = build_track_meta(path, root, fmt)
            (modified if entry else added).append(relative)
        if not row.artwork_scanned:
            locator = scan_artwork(path, root, fmt, folder_art_cache)
            row.artwork_source = row.artwork_format = row.artwork_offset = 0
            row.artwork_size = row.artwork_flags = 0
            row.artwork_width = row.artwork_height = row.artwork_picture_type = 0
            row.artwork_path, row.artwork_mtime = "", 0
            if locator:
                (row.artwork_source, row.artwork_format, row.artwork_offset,
                 row.artwork_size, row.artwork_flags, row.artwork_width,
                 row.artwork_height, row.artwork_picture_type,
                 row.artwork_path, row.artwork_mtime) = locator
            row.artwork_scanned = True
            artwork_scanned += 1
        # External LRC changes independently of the song; stat it even when tags are cached.
        # Keep the exact file case so DSM's case-sensitive URL resolves correctly.
        row.lyrics_path = ""
        for suffix in (".lrc", ".LRC"):
            candidate = path.with_suffix(suffix)
            if candidate.is_file():
                lrc_stat = candidate.stat()
                if 0 < lrc_stat.st_size <= 256 * 1024:
                    row.lyrics_path = "/" + candidate.relative_to(root).as_posix()
                break
        # Protect against concurrent SMB source edits while scanning.
        last = path.stat()
        if (last.st_size, last.st_mtime_ns) != (st.st_size, st.st_mtime_ns):
            raise RuntimeError(f"扫描过程中歌曲发生变化，请重新扫描：{relative}")
        rows.append(row)
        new_entries[relative] = {"size": st.st_size, "mtime_ns": st.st_mtime_ns, "track": asdict(row)}
        if n % 25 == 0 or n == len(paths):
            print(f"FAKEPOD_PROGRESS META {n} {len(paths)}", flush=True)
        if n % 250 == 0 or n == len(paths):
            print(f"metadata scan: {n}/{len(paths)} reused={len(unchanged)} parsed={len(added)+len(modified)}", flush=True)
    deleted = sorted(set(old) - set(new_entries), key=ascii_fold_key)
    mode = "incremental" if previous else "baseline"
    report = {"version": 1, "mode": mode, "total": len(rows),
              "added": added if previous else [], "modified": modified if previous else [],
              "removed": deleted if previous else [], "unchanged": len(unchanged),
              "initial": len(rows) if not previous else 0,
              "prepare_paths": ([p for p in (added + modified) if Path(p).suffix.lower() in (".mp3", ".flac")]
                                if previous else [t.path for t in rows if Path(t.path).suffix.lower() in (".mp3", ".flac")]),
              "remove_aliases": ([f"{stable_track_id(p):016X}{Path(p).suffix}" for p in deleted
                                  if Path(p).suffix.lower() in (".mp3", ".flac")]),
              "id": uuid.uuid4().hex}
    # Extra manually repaired aliases remain untouched. Deletion only considers
    # old canonical names tracked in our own validated cache, never unknown files.
    new_aliases = {f"{t.track_id:016X}{Path(t.path).suffix}" for t in rows
                   if Path(t.path).suffix.lower() in (".mp3", ".flac")}
    report["remove_aliases"] = [x for x in report["remove_aliases"] if x not in new_aliases]
    cache = {"version": CACHE_VERSION, "root": cache_fingerprint(root), "entries": new_entries}
    print("FAKEPOD_INCREMENTAL " + " ".join(f"{k}={v}" for k, v in
          (("mode", mode), ("total", len(rows)), ("added", len(report["added"])),
           ("modified", len(report["modified"])), ("removed", len(report["removed"])),
           ("reused", len(unchanged)))), flush=True)
    return rows, cache, report


def iter_tracks(root: Path) -> List[TrackMeta]:
    """Legacy full-scan API retained for callers and compatibility tests."""
    rows, _, _ = scan_incremental(root, None)
    return rows


def build_incremental_link_script(tracks: List[TrackMeta], report: dict) -> bytes:
    """BusyBox script with explicit prepare/cleanup phases, never deletes unknown aliases."""
    by_path = {row.path: row for row in tracks}
    revision = report["revision"]
    nonce = report["id"]
    marker = ".fakepod_prepared_" + nonce + ".ok"
    report["prepare_marker"] = marker
    lines = [
        "#!/bin/sh", "set -eu",
        # DSM 5.2 non-interactive SSH commands may not load the login shell PATH.
        # Prefix standard NAS locations without discarding vendor-added entries.
        'PATH="/bin:/usr/bin:/sbin:/usr/sbin:/usr/syno/bin:${PATH:-}"',
        'export PATH',
        'MODE="${1:-}"', 'MUSIC_ROOT="${2:-/volume1/music}"',
        'TRACK_ROOT="${3:-$MUSIC_ROOT/_FakePod/track}"',
        'INDEX_ROOT="$MUSIC_ROOT/_FakePod/music-index"',
        'MARKER="$INDEX_ROOT/' + marker + '"',
        'case "$MODE" in prepare|cleanup) ;; *) echo "Usage: $0 prepare|cleanup MUSIC_ROOT TRACK_ROOT"; exit 2;; esac',
        '[ -d "$MUSIC_ROOT" ] || { echo "Music root not found"; exit 1; }',
        '[ -d "$INDEX_ROOT" ] || { echo "Index folder not found"; exit 1; }',
        'mkdir -p "$TRACK_ROOT"',
        'if [ "$MODE" = prepare ]; then',
        # The old BusyBox ash non-interactive shell may fail `command -v ls`
        # even when `ls -di` works. Probe the actual operations instead.
        '  ls -di "$MUSIC_ROOT" >/dev/null 2>&1 || { echo "NAS ls -i unsupported or PATH invalid"; exit 1; }',
        "  awk 'BEGIN {print 1}' >/dev/null 2>&1 || { echo 'NAS awk operation failed'; exit 1; }",
        "  nas_inode() { ls -di \"$1\" | awk 'NR==1 {print $1}'; }",
        '  echo "Preflight: checking changed/new audio sources"',
    ]
    prepared = [by_path[p] for p in report["prepare_paths"]]
    for row in prepared:
        lines.append('  [ -f "$MUSIC_ROOT"/' + shell_single_quote(row.path.lstrip('/')) +
                     ' ] || { echo "Audio source missing"; exit 1; }')
    lines += ['  echo "Preparing changed/new short-ID aliases"']
    for row in prepared:
        src = '"$MUSIC_ROOT"/' + shell_single_quote(row.path.lstrip('/'))
        name = f"{row.track_id:016X}{Path(row.path).suffix}"
        dest = '"$TRACK_ROOT"/' + shell_single_quote(name)
        tmp = '"$TRACK_ROOT"/' + shell_single_quote('.' + name + '.incremental-tmp')
        lines += [
            '  SRC=' + src,
            '  DST=' + dest,
            '  TMP=' + tmp,
            '  [ ! -d "$DST" ] || { echo "Alias path is a directory"; exit 1; }',
            '  SAME_INODE=0',
            '  if [ -f "$DST" ]; then',
            '    SRC_INO=$(nas_inode "$SRC")',
            '    DST_INO=$(nas_inode "$DST")',
            '    [ -n "$SRC_INO" ] && [ -n "$DST_INO" ] || { echo "Cannot read inode"; exit 1; }',
            '    [ "$SRC_INO" != "$DST_INO" ] || SAME_INODE=1',
            '  fi',
            '  if [ "$SAME_INODE" -eq 1 ]; then',
            '    : # Existing alias is already the correct hard link; leave intact.',
            '  else',
            '    [ ! -e "$TMP" ] || { echo "Temporary alias already exists"; exit 1; }',
            '    ln "$SRC" "$TMP"',
            '    mv -f "$TMP" "$DST"',
            '  fi',
        ]
    lines += [
        '  printf "%s\\n" ' + shell_single_quote(nonce) + ' > "$MARKER.tmp.$$"',
        '  mv "$MARKER.tmp.$$" "$MARKER"',
        '  echo "FakePod incremental prepare OK; changed aliases: ' + str(len(prepared)) + '; marker: ' + marker + '"',
        'else',
        '  [ -f "$MARKER" ] || { echo "Prepare marker missing"; exit 1; }',
        '  grep -Fqx ' + shell_single_quote('revision=' + revision) + ' "$INDEX_ROOT/fakepod_catalog.meta" || { echo "New V2 meta not published"; exit 1; }',
        '  QUAR="$MUSIC_ROOT/_FakePod/quarantine-' + nonce[:12] + '"',
    ]
    aliases = report["remove_aliases"]
    if aliases:
        lines += ['  mkdir -p "$QUAR"', '  echo "Quarantining removed canonical aliases; no permanent deletion"']
        for name in aliases:
            quoted = shell_single_quote(name)
            lines += [
                '  if [ -f "$TRACK_ROOT"/' + quoted + ' ]; then',
                '    [ ! -e "$QUAR"/' + quoted + ' ] || { echo "Quarantine collision"; exit 1; }',
                '    mv "$TRACK_ROOT"/' + quoted + ' "$QUAR"/' + quoted,
                '  fi',
            ]
    lines += ['  echo "FakePod incremental cleanup OK; quarantined candidates: ' + str(len(aliases)) + '"', 'fi', '']
    return '\n'.join(lines).encode('utf-8')


def pool_builder():
    data = bytearray(b"\0")
    offsets = {"": 0}

    def add(text: str) -> int:
        text = clean_text(text)
        if text in offsets:
            return offsets[text]
        encoded = text.encode("utf-8") + b"\0"
        off = len(data)
        data.extend(encoded)
        offsets[text] = off
        return off

    return data, add


def build_catalog(tracks: List[TrackMeta]):
    pool, add = pool_builder()

    artist_ids: Dict[str, int] = {}
    artist_names: List[str] = []

    def artist_id(name: str) -> int:
        name = clean_text(name)
        if not name:
            return INVALID_ID
        key = name.casefold()
        existing = artist_ids.get(key)
        if existing is not None:
            return existing
        idx = len(artist_names)
        artist_ids[key] = idx
        artist_names.append(name)
        return idx

    album_ids: Dict[Tuple[str, str], int] = {}
    album_rows: List[Tuple[str, str, int]] = []

    def album_id(title: str, display_artist: str) -> int:
        title = clean_text(title)
        if not title:
            return INVALID_ID
        display_artist = clean_text(display_artist)
        key = (title.casefold(), display_artist.casefold())
        existing = album_ids.get(key)
        if existing is not None:
            return existing
        aid = artist_id(display_artist)
        idx = len(album_rows)
        album_ids[key] = idx
        album_rows.append((title, display_artist, aid))
        return idx

    track_entity_rows = []
    for track in tracks:
        direct_artist_id = artist_id(track.artist)
        alb_id = album_id(track.album, track.album_artist)
        track_entity_rows.append((direct_artist_id, alb_id))

    artist_bytes = bytearray()
    for name in artist_names:
        artist_bytes.extend(ARTIST.pack(add(name), 0))

    album_bytes = bytearray()
    album_cover_track = {}
    for track_index, (track, (_, alb_id)) in enumerate(zip(tracks, track_entity_rows)):
        if alb_id != INVALID_ID and track.artwork_source and alb_id not in album_cover_track:
            album_cover_track[alb_id] = track_index
    for alb_index, (title, display_artist, aid) in enumerate(album_rows):
        album_bytes.extend(ALBUM.pack(
            add(title),
            add(display_artist),
            aid,
            album_cover_track.get(alb_index, INVALID_ID),
            0,
            0,
            0,
        ))

    artist_ref_bytes = bytearray()
    artwork_bytes = bytearray()
    lyrics_bytes = bytearray()
    track_bytes = bytearray()
    manifest_rows = bytearray()

    for index, track in enumerate(tracks):
        direct_artist_id, alb_id = track_entity_rows[index]
        artist_ref_start = len(artist_ref_bytes) // ARTIST_REF.size
        artist_ref_count = 0
        if direct_artist_id != INVALID_ID:
            artist_ref_bytes.extend(ARTIST_REF.pack(direct_artist_id, 0))
            artist_ref_count = 1

        artwork_ref_id = INVALID_ID
        if track.artwork_source and track.artwork_size:
            artwork_ref_id = len(artwork_bytes) // ARTWORK.size
            artwork_path_off = add(track.artwork_path) if track.artwork_source == 3 else 0
            artwork_bytes.extend(ARTWORK.pack(
                track.artwork_offset, track.artwork_size, artwork_path_off,
                track.artwork_mtime if track.artwork_source == 3 else 0,
                track.artwork_flags, track.artwork_width, track.artwork_height,
                track.artwork_source, track.artwork_format,
                track.artwork_picture_type, 0,
            ))
        lyrics_ref_start = len(lyrics_bytes) // LYRICS.size
        lyrics_ref_count = 0
        if track.lyrics_path:
            # External + Synced, no embedded payload; keep LyricsRefV2 disk contract.
            lyrics_bytes.extend(LYRICS.pack(0, 0, add(track.lyrics_path), 0, 0, 1, 2, 0, 0))
            lyrics_ref_count = 1
        path_off = add(track.path)
        title_off = add(track.title)
        artist_off = add(track.artist)
        track_bytes.extend(
            TRACK.pack(
                path_off,
                title_off,
                artist_off,
                artist_ref_start,
                artist_ref_count,
                0,
                lyrics_ref_start, lyrics_ref_count, 0,
                alb_id,
                artwork_ref_id,
                track.metadata_flags,
                track.track_number,
                track.track_total,
                track.disc_number,
                track.disc_total,
                track.release_year,
                track.original_year,
                track.fmt,
                0, 0, 0,
                0,
                track.size,
                0, 0, 0,
                0,
                0, 0, 0,
                0,
                0,
                0, 0,
            )
        )
        manifest_rows.extend(MANIFEST_ROW.pack(index, track.fmt, track.size, track.mtime))

    if len(pool) > 32 * 1024 * 1024:
        raise ValueError(f"string pool too large for FakePod V2: {len(pool)} bytes")

    sections_payload = [
        bytes(pool),
        bytes(artist_bytes),
        bytes(album_bytes),
        bytes(artist_ref_bytes),
        bytes(lyrics_bytes),
        bytes(artwork_bytes),
        bytes(track_bytes),
    ]
    section_types = [
        SEC_STR_POOL,
        SEC_ARTISTS,
        SEC_ALBUMS,
        SEC_TRACK_ARTIST_REFS,
        SEC_LYRICS_REFS,
        SEC_ARTWORK_REFS,
        SEC_TRACKS,
    ]
    row_sizes = [1, ARTIST.size, ALBUM.size, ARTIST_REF.size, LYRICS.size, ARTWORK.size, TRACK.size]
    counts = [
        len(pool),
        len(artist_names),
        len(album_rows),
        len(artist_ref_bytes) // ARTIST_REF.size,
        len(lyrics_bytes) // LYRICS.size,
        len(artwork_bytes) // ARTWORK.size,
        len(tracks),
    ]

    offset = CATALOG_HEADER.size + SECTION_COUNT * SECTION.size
    sections = bytearray()
    for section_type, payload, count, row_size in zip(section_types, sections_payload, counts, row_sizes):
        sections.extend(SECTION.pack(section_type, offset, len(payload), count, row_size, 0, crc32(payload)))
        offset += len(payload)

    payload = b"".join(sections_payload)
    index_crc = crc32(payload)
    header = CATALOG_HEADER.pack(
        b"FPCATV2\0",
        CATALOG_VERSION,
        CATALOG_HEADER.size,
        SECTION.size,
        SECTION_COUNT,
        offset,
        index_crc,
        SIGNATURE_MODE_FAST,
        len(tracks),
        len(artist_names),
        len(album_rows),
        len(artist_ref_bytes) // ARTIST_REF.size,
        len(lyrics_bytes) // LYRICS.size,
        len(artwork_bytes) // ARTWORK.size,
    )
    index_file = header + bytes(sections) + payload

    manifest_payload = bytes(manifest_rows)
    manifest_crc = crc32(manifest_payload)
    manifest_header = MANIFEST_HEADER.pack(
        b"FPMNFV2\0",
        MANIFEST_VERSION,
        MANIFEST_HEADER.size,
        MANIFEST_ROW.size,
        0,
        len(tracks),
        index_crc,
        manifest_crc,
        SIGNATURE_MODE_FAST,
    )
    manifest_file = manifest_header + manifest_payload
    return index_file, manifest_file, index_crc, manifest_crc, len(artist_names), len(album_rows)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build FakePod V2 NAS catalog files")
    parser.add_argument("music_root", type=Path, help="Root folder containing NAS music")
    parser.add_argument("output_dir", type=Path, help="Folder served over HTTP/WebDAV")
    parser.add_argument("--cache-from", type=Path, help="Published scan cache for incremental metadata reuse")
    parser.add_argument("--force-full", action="store_true", help="Ignore previous scan cache; rebuild baseline")
    args = parser.parse_args()

    root = args.music_root.resolve()
    out = args.output_dir.resolve()
    if not root.is_dir():
        parser.error(f"music_root is not a directory: {root}")
    out.mkdir(parents=True, exist_ok=True)

    cache_source = None if args.force_full else args.cache_from
    prior = read_scan_cache(cache_source, root)
    tracks, scan_cache, report = scan_incremental(root, prior)
    print("FAKEPOD_PROGRESS CATALOG 0 1", flush=True)
    index_file, manifest_file, index_crc, manifest_crc, artist_count, album_count = build_catalog(tracks)

    # R46.0.96: no Python/Docker runtime on DSM. Emit a BusyBox-compatible hard-link
    # builder; the V2 catalog row layout still does not gain a per-track ID field.
    alias_script = build_static_alias_script(tracks)
    alias_crc = crc32(alias_script)
    # Force a metadata transition from R46.0.95 dynamic-map catalogs even when
    # index/manifest bytes are unchanged.
    revision = (((index_crc << 32) | manifest_crc) ^ ((alias_crc << 1) | 0x96)) & 0xFFFFFFFFFFFFFFFF
    if revision == 0:
        revision = 1
    report["revision"] = f"0x{revision:016X}"
    (out / CACHE_NAME).write_text(json.dumps(scan_cache, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    incremental = build_incremental_link_script(tracks, report)
    (out / INCREMENTAL_SCRIPT_NAME).write_bytes(incremental)
    (out / REPORT_NAME).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    (out / "music_index_v2.bin").write_bytes(index_file)
    (out / "music_manifest_v2.bin").write_bytes(manifest_file)
    (out / "fakepod_make_static_track_links.sh").write_bytes(alias_script)

    stale_map = out / "fakepod_track_map_v1.json"
    if stale_map.exists():
        stale_map.unlink()

    meta = (
        "FAKEPOD_NAS_V1\n"
        f"catalog_version={CATALOG_VERSION}\n"
        f"revision=0x{revision:016X}\n"
        f"tracks={len(tracks)}\n"
        f"artists={artist_count}\n"
        f"albums={album_count}\n"
        f"index_size={len(index_file)}\n"
        f"index_crc32=0x{index_crc:08X}\n"
        f"manifest_size={len(manifest_file)}\n"
        f"manifest_crc32=0x{manifest_crc:08X}\n"
        "track_alias_version=1\n"
        f"track_alias_script_size={len(alias_script)}\n"
        f"track_alias_script_crc32=0x{alias_crc:08X}\n"
    )
    (out / "fakepod_catalog.meta").write_text(meta, encoding="utf-8", newline="\n")

    title_tagged = sum(1 for t in tracks if t.metadata_flags & META_HAS_TITLE_TAG)
    artist_tagged = sum(1 for t in tracks if t.metadata_flags & META_HAS_ARTIST_TAG)
    album_tagged = sum(1 for t in tracks if t.metadata_flags & META_HAS_ALBUM_TAG)
    year_tagged = sum(1 for t in tracks if t.release_year or t.original_year)
    print(f"FakePod NAS Catalog: tracks={len(tracks)} artists={artist_count} albums={album_count}")
    print(f"metadata: title={title_tagged} artist={artist_tagged} album={album_tagged} year={year_tagged}")
    print(f"NAS LRC refs={sum(bool(t.lyrics_path) for t in tracks)} (external synced)")
    print(f"artwork refs={sum(bool(t.artwork_source) for t in tracks)} "
          f"embedded={sum(t.artwork_source in (1, 2) for t in tracks)} "
          f"external={sum(t.artwork_source == 3 for t in tracks)}")
    print(f"index={len(index_file)} bytes crc=0x{index_crc:08X}")
    print(f"manifest={len(manifest_file)} bytes crc=0x{manifest_crc:08X}")
    alias_count = sum(1 for t in tracks if Path(t.path).suffix.lower() in (".mp3", ".flac"))
    print(f"static_alias_script={len(alias_script)} bytes crc=0x{alias_crc:08X} links={alias_count} ids=FNV1A64 collisions=0")
    print("DSM: sh fakepod_make_static_track_links.sh /path/to/Music /path/to/Music/_FakePod/track")
    print(f"revision=0x{revision:016X}")
    print(f"incremental: {report['mode']} +{len(report['added'])} ~{len(report['modified'])} -{len(report['removed'])} unchanged={report['unchanged']}")
    print(f"output={out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
