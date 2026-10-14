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

# Keep these bit values byte-compatible with MediaTechnicalFlags in
# src/media/library/media_types.h.  NAS V2 already stores the full technical
# payload in each 124-byte track row; older NAS builders simply wrote zeros.
TECH_PARSED = 1 << 0
TECH_HAS_VBR_HEADER = 1 << 2
TECH_DURATION_ESTIMATED = 1 << 3

MP3_SCAN_BUFFER_BYTES = 8192
MP3_MAX_LEADING_SCAN_BYTES = 256 * 1024
FLAC_MAX_METADATA_BLOCK_COUNT = 256

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
        if track.lyrics_path:
            lrc_src = shell_single_quote(track.lyrics_path.lstrip("/"))
            lrc_alias = shell_single_quote(f"{track.track_id:016X}.lrc")
            lines.append(f'ln "$MUSIC_ROOT"/{lrc_src} "$TMP"/{lrc_alias}')
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


def read_be24(data: bytes) -> int:
    return int.from_bytes(data, "big", signed=False)


def parse_mp3_frame_header(header: bytes) -> Optional[dict]:
    """Mirror the local media_probe.cpp Layer III header parser."""
    if len(header) < 4 or header[0] != 0xFF or (header[1] & 0xE0) != 0xE0:
        return None
    version_id = (header[1] >> 3) & 0x03
    layer = (header[1] >> 1) & 0x03
    if version_id == 1 or layer != 1:
        return None
    bitrate_index = (header[2] >> 4) & 0x0F
    sample_index = (header[2] >> 2) & 0x03
    if bitrate_index in (0, 15) or sample_index == 3:
        return None

    bitrate_mpeg1 = (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0)
    bitrate_mpeg2 = (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0)
    mpeg1_rates = (44100, 48000, 32000)
    sample_rate = mpeg1_rates[sample_index]
    if version_id == 2:
        sample_rate //= 2
    elif version_id == 0:
        sample_rate //= 4

    mpeg1 = version_id == 3
    bitrate = (bitrate_mpeg1 if mpeg1 else bitrate_mpeg2)[bitrate_index]
    padding = (header[2] >> 1) & 0x01
    frame_size = ((144000 if mpeg1 else 72000) * bitrate) // sample_rate + padding
    if frame_size < 24:
        return None
    return {
        "sample_rate_hz": sample_rate,
        "bitrate_kbps": bitrate,
        "frame_size_bytes": frame_size,
        "samples_per_frame": 1152 if mpeg1 else 576,
        "channels": 1 if ((header[3] >> 6) & 0x03) == 3 else 2,
        "version_id": version_id,
        "has_crc": (header[1] & 0x01) == 0,
    }


def mp3_headers_compatible(a: dict, b: dict) -> bool:
    return (a["sample_rate_hz"] == b["sample_rate_hz"] and
            a["samples_per_frame"] == b["samples_per_frame"] and
            a["version_id"] == b["version_id"])


def find_mp3_first_frame(f, search_start: int, file_size: int) -> Tuple[int, Optional[dict]]:
    search_limit = min(file_size, search_start + MP3_MAX_LEADING_SCAN_BYTES)
    cursor = search_start
    while cursor + 4 <= search_limit:
        f.seek(cursor)
        data = f.read(min(MP3_SCAN_BUFFER_BYTES, search_limit - cursor))
        if len(data) < 4:
            break
        for i in range(len(data) - 3):
            first = parse_mp3_frame_header(data[i:i + 4])
            if first is None:
                continue
            frame_offset = cursor + i
            next_offset = frame_offset + first["frame_size_bytes"]
            if next_offset + 4 > file_size:
                continue
            f.seek(next_offset)
            second = parse_mp3_frame_header(f.read(4))
            if second is not None and mp3_headers_compatible(first, second):
                return frame_offset, first
        if len(data) < min(MP3_SCAN_BUFFER_BYTES, search_limit - cursor):
            break
        cursor += max(1, len(data) - 3)
    return 0, None


def mp3_side_info_size(header: dict) -> int:
    if header["version_id"] == 3:
        return 17 if header["channels"] == 1 else 32
    return 9 if header["channels"] == 1 else 17


def mp3_xing_frame_count(f, frame_offset: int, header: dict) -> int:
    offset = frame_offset + 4 + (2 if header["has_crc"] else 0) + mp3_side_info_size(header)
    f.seek(offset)
    prefix = f.read(12)
    if len(prefix) != 12 or prefix[:4] not in (b"Xing", b"Info"):
        return 0
    flags = int.from_bytes(prefix[4:8], "big", signed=False)
    return int.from_bytes(prefix[8:12], "big", signed=False) if flags & 1 else 0


def mp3_vbri_frame_count(f, frame_offset: int) -> int:
    f.seek(frame_offset + 36)
    header = f.read(18)
    if len(header) != 18 or header[:4] != b"VBRI":
        return 0
    return int.from_bytes(header[14:18], "big", signed=False)


def probe_mp3_technical(path: Path, file_size: int) -> dict:
    with path.open("rb") as f:
        first10 = f.read(10)
        if len(first10) < 4:
            return {}
        search_start = 0
        if first10[:3] == b"ID3":
            if len(first10) < 10 or any(b & 0x80 for b in first10[6:10]):
                return {}
            search_start = 10 + syncsafe32(first10[6:10]) + (10 if first10[5] & 0x10 else 0)
        frame_offset, frame = find_mp3_first_frame(f, search_start, file_size)
        if frame is None:
            return {}

        flags = TECH_PARSED
        total_frames = 0
        duration_ms = 0
        frame_count = mp3_xing_frame_count(f, frame_offset, frame)
        if frame_count == 0:
            frame_count = mp3_vbri_frame_count(f, frame_offset)
        if frame_count:
            flags |= TECH_HAS_VBR_HEADER
            total_frames = frame_count * frame["samples_per_frame"]
            duration_ms = (total_frames * 1000) // frame["sample_rate_hz"]
        elif frame["bitrate_kbps"] and file_size > frame_offset:
            # Same conservative fallback as the local probe: duration is marked
            # estimated because a VBR file without Xing/VBRI cannot be exact.
            duration_ms = ((file_size - frame_offset) * 8) // frame["bitrate_kbps"]
            total_frames = (duration_ms * frame["sample_rate_hz"]) // 1000
            flags |= TECH_DURATION_ESTIMATED

        return {
            "technical_flags": flags,
            "sample_rate_hz": frame["sample_rate_hz"],
            "bitrate_kbps": frame["bitrate_kbps"],
            "duration_ms": min(duration_ms, 0xFFFFFFFF),
            "total_frames": total_frames,
            "audio_data_offset": frame_offset,
            "metadata_end_offset": frame_offset,
            "max_frame_size": 0,
            "max_block_size": 0,
            "samples_per_frame": frame["samples_per_frame"],
            "channels": frame["channels"],
            "bits_per_sample": 16,
        }


def probe_flac_technical(path: Path) -> dict:
    with path.open("rb") as f:
        first10 = f.read(10)
        if len(first10) < 4:
            return {}
        flac_offset = 0
        if first10[:3] == b"ID3":
            if len(first10) < 10 or any(b & 0x80 for b in first10[6:10]):
                return {}
            flac_offset = 10 + syncsafe32(first10[6:10]) + (10 if first10[5] & 0x10 else 0)
        f.seek(flac_offset)
        if f.read(4) != b"fLaC":
            return {}

        result = {}
        last = False
        block_count = 0
        while not last:
            block_count += 1
            if block_count > FLAC_MAX_METADATA_BLOCK_COUNT:
                return {}
            header = f.read(4)
            if len(header) != 4:
                return {}
            last = bool(header[0] & 0x80)
            block_type = header[0] & 0x7F
            length = read_be24(header[1:4])
            if block_type == 0:
                if length != 34:
                    return {}
                streaminfo = f.read(34)
                if len(streaminfo) != 34:
                    return {}
                sample_rate = ((streaminfo[10] << 12) |
                               (streaminfo[11] << 4) |
                               (streaminfo[12] >> 4))
                channels = ((streaminfo[12] >> 1) & 0x07) + 1
                bits = (((streaminfo[12] & 0x01) << 4) | (streaminfo[13] >> 4)) + 1
                total_frames = ((streaminfo[13] & 0x0F) << 32) | int.from_bytes(streaminfo[14:18], "big")
                if sample_rate == 0 or channels == 0:
                    return {}
                result = {
                    "technical_flags": TECH_PARSED,
                    "sample_rate_hz": sample_rate,
                    "bitrate_kbps": 0,
                    "duration_ms": min((total_frames * 1000) // sample_rate, 0xFFFFFFFF),
                    "total_frames": total_frames,
                    "max_frame_size": read_be24(streaminfo[7:10]),
                    "max_block_size": int.from_bytes(streaminfo[2:4], "big"),
                    "samples_per_frame": 0,
                    "channels": channels,
                    "bits_per_sample": bits,
                }
            else:
                f.seek(length, os.SEEK_CUR)

        if not result:
            return {}
        audio_offset = f.tell()
        result["audio_data_offset"] = audio_offset
        result["metadata_end_offset"] = audio_offset
        return result


def probe_technical(path: Path, fmt: int, file_size: int) -> dict:
    try:
        if fmt == FORMAT_MAP[".mp3"]:
            return probe_mp3_technical(path, file_size)
        if fmt == FORMAT_MAP[".flac"]:
            return probe_flac_technical(path)
    except (OSError, ValueError, OverflowError):
        pass
    return {}


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
    # Byte-compatible MediaTechnicalInfo payload already present in TrackDiskRowV2.
    # Only MP3/FLAC are populated in this phase; other formats retain zeros.
    technical_flags: int = 0
    sample_rate_hz: int = 0
    bitrate_kbps: int = 0
    duration_ms: int = 0
    total_frames: int = 0
    audio_data_offset: int = 0
    metadata_end_offset: int = 0
    max_frame_size: int = 0
    max_block_size: int = 0
    samples_per_frame: int = 0
    channels: int = 0
    bits_per_sample: int = 0
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
    # Compatible with caches that predate artwork/LRC/technical locators. Core
    # identity/metadata fields remain mandatory so a damaged cache is rejected.
    optional = {
        "artwork_scanned", "artwork_source", "artwork_format", "artwork_offset",
        "artwork_size", "artwork_flags", "artwork_width", "artwork_height",
        "artwork_picture_type", "artwork_path", "artwork_mtime", "lyrics_path",
        "technical_flags", "sample_rate_hz", "bitrate_kbps", "duration_ms",
        "total_frames", "audio_data_offset", "metadata_end_offset",
        "max_frame_size", "max_block_size", "samples_per_frame", "channels",
        "bits_per_sample",
    }
    required = allowed - optional
    if (not isinstance(row, dict) or not required.issubset(row) or
            not set(row).issubset(allowed)):
        raise ValueError(f"损坏的扫描缓存：{path}")
    result = TrackMeta(**row)
    if result.path != path or result.track_id != stable_track_id(path):
        raise ValueError(f"扫描缓存曲目不匹配：{path}")
    return result


def matching_directory_lrc(song: Path, cache: dict) -> Optional[Path]:
    """Resolve the real-cased, same-directory/same-stem LRC once per folder.

    Windows SMB may treat ``song.lrc`` as present when the NAS actual filename
    is ``song.LRC``.  Constructing the lowercase path would then fail on the
    case-sensitive NAS SSH source, so enumerate actual directory entries.
    MP3 and FLAC with the same stem intentionally return the *same* source.
    """
    folder = song.parent
    if folder not in cache:
        mapping = {}
        for candidate in folder.iterdir():
            if candidate.suffix.lower() != ".lrc" or not candidate.is_file():
                continue
            mapping.setdefault(candidate.stem.casefold(), []).append(candidate)
        cache[folder] = mapping
    matches = cache[folder].get(song.stem.casefold(), [])
    exact = [p for p in matches if p.stem == song.stem]
    if len(exact) == 1:
        return exact[0]
    if len(matches) == 1:
        return matches[0]
    if matches:
        raise ValueError(f"同目录存在多个无法唯一匹配的LRC：{song} -> {matches}")
    return None


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
    folder_lyrics_cache = {}
    artwork_scanned = 0
    new_entries = {}
    added, modified, unchanged, prepare_lyrics = [], [], [], []
    technical_parsed = 0
    technical_failed = 0
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
        # Older scan caches have no technical payload. Refresh only that payload
        # once; do not classify the audio as modified and do not rebuild aliases.
        if fmt in (FORMAT_MAP[".mp3"], FORMAT_MAP[".flac"]) and not (row.technical_flags & TECH_PARSED):
            technical = probe_technical(path, fmt, st.st_size)
            if technical:
                for name, value in technical.items():
                    setattr(row, name, value)
                technical_parsed += 1
            else:
                technical_failed += 1
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
        # Enumerate real directory entries rather than probing guessed .lrc/.LRC
        # spellings: a case-insensitive SMB client can otherwise invent a path
        # which is absent on the NAS case-sensitive SSH filesystem.
        row.lyrics_path = ""
        candidate = matching_directory_lrc(path, folder_lyrics_cache)
        if candidate is not None:
            lrc_stat = candidate.stat()
            if 0 < lrc_stat.st_size <= 256 * 1024:
                row.lyrics_path = "/" + candidate.relative_to(root).as_posix()
                if fmt in (FORMAT_MAP[".mp3"], FORMAT_MAP[".flac"]):
                    alias = root / "_FakePod" / "track" / f"{row.track_id:016X}.lrc"
                    # Each format keeps its own short ID, but both short links
                    # may point to this one, shared physical LRC file.
                    if not alias.is_file() or not os.path.samefile(candidate, alias):
                        prepare_lyrics.append(relative)
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
              "prepare_lyrics": prepare_lyrics,
              "remove_aliases": ([f"{stable_track_id(p):016X}{Path(p).suffix}" for p in deleted
                                  if Path(p).suffix.lower() in (".mp3", ".flac")]
                                 + [f"{stable_track_id(p):016X}.lrc" for p in deleted
                                    if Path(p).suffix.lower() in (".mp3", ".flac")
                                    and old[p].get("track", {}).get("lyrics_path")]
                                 + [f"{t.track_id:016X}.lrc" for t in rows
                                    if Path(t.path).suffix.lower() in (".mp3", ".flac")
                                    and not t.lyrics_path and t.path in old
                                    and old[t.path].get("track", {}).get("lyrics_path")]),
              "id": uuid.uuid4().hex}
    # Extra manually repaired aliases remain untouched. Deletion only considers
    # old canonical names tracked in our own validated cache, never unknown files.
    new_aliases = {f"{t.track_id:016X}{Path(t.path).suffix}" for t in rows
                   if Path(t.path).suffix.lower() in (".mp3", ".flac")}
    new_aliases |= {f"{t.track_id:016X}.lrc" for t in rows if t.lyrics_path
                    and Path(t.path).suffix.lower() in (".mp3", ".flac")}
    report["remove_aliases"] = [x for x in report["remove_aliases"] if x not in new_aliases]
    cache = {"version": CACHE_VERSION, "root": cache_fingerprint(root), "entries": new_entries}
    print("FAKEPOD_INCREMENTAL " + " ".join(f"{k}={v}" for k, v in
          (("mode", mode), ("total", len(rows)), ("added", len(report["added"])),
           ("modified", len(report["modified"])), ("removed", len(report["removed"])),
           ("reused", len(unchanged)))), flush=True)
    if technical_parsed or technical_failed:
        print(f"FAKEPOD_TECH_SCAN parsed={technical_parsed} failed={technical_failed}", flush=True)
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
        '  echo "Preflight: checking changed/new audio/LRC sources"',
    ]
    prepared = [by_path[p] for p in report["prepare_paths"]]
    lyric_prepared = [by_path[p] for p in report["prepare_lyrics"]]
    # LRC alias uses the AUDIO track's stable ID, not the LRC path's own hash.
    # This lets LyricsRefV2 point to the same proven _FakePod/track route.
    aliases = [(row.path, f"{row.track_id:016X}{Path(row.path).suffix}") for row in prepared]
    aliases.extend((row.lyrics_path, f"{row.track_id:016X}.lrc") for row in lyric_prepared)
    # The Windows SMB scan can see a file that the NAS SSH path cannot (e.g.
    # a different /volumeN share mapping). Report the *first exact source* so
    # the operator can diagnose it without changing any existing aliases.
    for rel, _ in aliases:
        src = '"$MUSIC_ROOT"/' + shell_single_quote(rel.lstrip('/'))
        lines += [
            '  if [ ! -f ' + src + ' ]; then',
            '    echo "ERROR: Audio/LRC source missing or not a regular file:" >&2',
            '    printf "  SSH path: %s\\n" ' + src + ' >&2',
            '    echo "  Music root: $MUSIC_ROOT" >&2',
            '    echo "  Check NAS /volumeN, SMB share mapping, case and permissions." >&2',
            '    echo "  Parent/source listing:" >&2',
            '    ls -ld "$(dirname ' + src + ')" ' + src + ' >&2 || :',
            '    exit 1',
            '  fi',
        ]
    lines += ['  echo "Preparing changed/new audio and LRC short-ID aliases"']
    for rel, name in aliases:
        src = '"$MUSIC_ROOT"/' + shell_single_quote(rel.lstrip('/'))
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
        '  echo "FakePod incremental prepare OK; changed audio: ' + str(len(prepared)) + '; lyrics: ' + str(len(lyric_prepared)) + '; marker: ' + marker + '"',
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
            # External + Synced; _FakePod/track has the same WebDAV ACL as audio.
            # Keep 28-byte LyricsRefV2 and catalog version unchanged. All supported
            # MP3/FLAC LRC files are linked before the V2 index can be published.
            lrc_path = (f"/_FakePod/track/{track.track_id:016X}.lrc"
                        if track.fmt in (FORMAT_MAP[".mp3"], FORMAT_MAP[".flac"])
                        else track.lyrics_path)
            lyrics_bytes.extend(LYRICS.pack(0, 0, add(lrc_path), 0, 0, 1, 2, 0, 0))
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
                track.channels,
                track.bits_per_sample,
                0,
                track.technical_flags,
                track.size,
                track.sample_rate_hz,
                track.bitrate_kbps,
                track.duration_ms,
                track.total_frames,
                track.audio_data_offset,
                track.metadata_end_offset,
                0,
                0,
                track.max_frame_size,
                track.max_block_size,
                track.samples_per_frame,
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

    # The GUI stages long NAS scans under a disposable directory.  Re-create it
    # immediately before committing generated artifacts in case the directory
    # disappeared while the scan was running (for example, an external temp
    # cleanup).  This is safe because the stage contains no published data yet.
    out.mkdir(parents=True, exist_ok=True)
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
    mp3_tracks = [t for t in tracks if t.fmt == FORMAT_MAP[".mp3"]]
    flac_tracks = [t for t in tracks if t.fmt == FORMAT_MAP[".flac"]]
    mp3_parsed = sum(bool(t.technical_flags & TECH_PARSED) for t in mp3_tracks)
    flac_parsed = sum(bool(t.technical_flags & TECH_PARSED) for t in flac_tracks)
    mp3_vbr = sum(bool(t.technical_flags & TECH_HAS_VBR_HEADER) for t in mp3_tracks)
    mp3_estimated = sum(bool(t.technical_flags & TECH_DURATION_ESTIMATED) for t in mp3_tracks)
    print(f"technical MP3: parsed={mp3_parsed}/{len(mp3_tracks)} vbr_header={mp3_vbr} estimated={mp3_estimated}")
    print(f"technical FLAC: parsed={flac_parsed}/{len(flac_tracks)}")
    lyric_paths = [t.lyrics_path for t in tracks if t.lyrics_path]
    print(f"NAS LRC refs={len(lyric_paths)} (external synced, short-ID for MP3/FLAC)")
    print(f"NAS LRC shared={len(lyric_paths) - len(set(lyric_paths))} (extra songs reusing same-directory LRC)")
    print(f"NAS LRC alias prepare={len(report['prepare_lyrics'])} (SSH before publish if nonzero)")
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
