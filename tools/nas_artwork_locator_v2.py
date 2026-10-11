"""Bounded NAS-side artwork locator scanning; no album bytes are saved to V2.

Only physical MP3 APIC/ID3v2.2 PIC and FLAC PICTURE offsets, or JPEG/PNG
cover/folder/front files are returned. Opus picture is intentionally deferred.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Optional

ARTWORK_MAX = 2 * 1024 * 1024
ID3_MAX = 8 * 1024 * 1024
JPEG_SCAN_LIMIT = 128 * 1024


def _syncsafe(raw: bytes) -> int:
    return ((raw[0] & 127) << 21) | ((raw[1] & 127) << 14) | ((raw[2] & 127) << 7) | (raw[3] & 127)


def _image_info(stream, offset: int, length: int):
    if not (0 < length <= ARTWORK_MAX):
        return None
    stream.seek(offset)
    head = stream.read(min(length, JPEG_SCAN_LIMIT))
    if len(head) >= 24 and head[:8] == b"\x89PNG\r\n\x1a\n" and head[12:16] == b"IHDR":
        width, height = int.from_bytes(head[16:20], "big"), int.from_bytes(head[20:24], "big")
        if 0 < width <= 65535 and 0 < height <= 65535:
            return 2, width, height
    if head[:2] != b"\xff\xd8":
        return None
    pos = 2
    while pos + 4 <= len(head):
        if head[pos] != 255:
            pos += 1
            continue
        while pos < len(head) and head[pos] == 255:
            pos += 1
        if pos >= len(head):
            break
        marker = head[pos]
        pos += 1
        if marker in (0xD8, 0xD9, 0x01) or 0xD0 <= marker <= 0xD7:
            continue
        if pos + 2 > len(head):
            break
        seg_size = int.from_bytes(head[pos:pos+2], "big")
        if seg_size < 2 or pos + seg_size > len(head):
            break
        if marker in (0xC0,0xC1,0xC2,0xC3,0xC5,0xC6,0xC7,0xC9,0xCA,0xCB,0xCD,0xCE,0xCF) and seg_size >= 7:
            height = int.from_bytes(head[pos+3:pos+5], "big")
            width = int.from_bytes(head[pos+5:pos+7], "big")
            if width and height:
                return 1, width, height
        if marker == 0xDA:
            break
        pos += seg_size
    # Decoder will check actual dimensions; keep jpeg with truncated/complex headers.
    return 1, 0, 0


def _id3_picture_payload_head(stream, payload_start: int, size: int, pic_v22: bool):
    stream.seek(payload_start)
    head = stream.read(min(size, 8192))
    if len(head) < 6:
        return None
    enc = head[0]
    if enc not in (0, 1, 2, 3):
        return None
    if pic_v22:
        typ_pos = 4
        if head[1:4].upper() not in (b"JPG", b"PNG"):
            return None
    else:
        zero = head.find(b"\0", 1)
        if zero < 0:
            return None
        typ_pos = zero + 1
    if typ_pos >= len(head):
        return None
    pic_type = head[typ_pos]
    start = typ_pos + 1
    if enc in (0, 3):
        end = head.find(b"\0", start)
        if end < 0:
            return None
        picture_at = end + 1
    else:
        # Enc=1 often contains UTF-16 BOM; the descriptor ends at aligned UTF-16 NUL.
        end = -1
        for i in range(start, len(head) - 1, 2):
            if head[i:i+2] == b"\0\0":
                end = i
                break
        if end < 0:
            return None
        picture_at = end + 2
    if picture_at >= size:
        return None
    return pic_type, picture_at


def scan_mp3(path: Path):
    try:
        with path.open("rb") as stream:
            header = stream.read(10)
            if len(header) < 10 or header[:3] != b"ID3" or header[3] not in (2, 3, 4):
                return None
            ver = header[3]
            flags = header[5]
            size = _syncsafe(header[6:10])
            if size > ID3_MAX:
                return None
            end = min(path.stat().st_size, size + 10)
            pos = 10
            if flags & 0x40 and ver in (3, 4):
                stream.seek(pos)
                extended = stream.read(4)
                if len(extended) != 4:
                    return None
                pos += (4 + int.from_bytes(extended, "big")) if ver == 3 else _syncsafe(extended)
            winner = None
            while pos < end:
                stream.seek(pos)
                frame_header = stream.read(6 if ver == 2 else 10)
                if len(frame_header) < (6 if ver == 2 else 10) or frame_header[0] == 0:
                    break
                if ver == 2:
                    frame_id = frame_header[:3]
                    length = int.from_bytes(frame_header[3:6], "big")
                    payload_start = pos + 6
                else:
                    frame_id = frame_header[:4]
                    length = _syncsafe(frame_header[4:8]) if ver == 4 else int.from_bytes(frame_header[4:8], "big")
                    payload_start = pos + 10
                if length <= 0 or payload_start + length > end:
                    break
                if frame_id in (b"PIC", b"APIC") and length <= ARTWORK_MAX + 8192:
                    # ID3v2.4 frame-level compression/encryption/DLI changes the payload; skip.
                    if ver == 4 and (frame_header[9] & (0x08 | 0x04 | 0x01)):
                        pos = payload_start + length
                        continue
                    if ver == 3 and (frame_header[9] & (0x80 | 0x40)):
                        pos = payload_start + length
                        continue
                    info = _id3_picture_payload_head(stream, payload_start, length, ver == 2)
                    if info:
                        picture_type, inner_offset = info
                        data_offset = payload_start + inner_offset
                        data_size = length - inner_offset
                        if data_size <= ARTWORK_MAX:
                            detected = _image_info(stream, data_offset, data_size)
                            if detected:
                                fmt, w, h = detected
                                # ID3 tag/frame unsynchronization: physical offsets are retained.
                                unsync = bool(flags & 0x80) or bool(ver == 4 and frame_header[9] & 0x02)
                                ref = (1, fmt, data_offset, data_size, 1 if unsync else 0, w, h, picture_type)
                                if winner is None or (picture_type == 3 and winner[-1] != 3):
                                    winner = ref
                                    if picture_type == 3:
                                        break
                pos = payload_start + length
            return winner
    except OSError:
        return None


def scan_flac(path: Path):
    try:
        with path.open("rb") as stream:
            if stream.read(4) != b"fLaC":
                return None
            total = path.stat().st_size
            pos = 4
            winner = None
            while pos + 4 <= total:
                stream.seek(pos)
                h = stream.read(4)
                is_last = (h[0] & 0x80) != 0
                block_type = h[0] & 0x7F
                length = int.from_bytes(h[1:4], "big")
                base = pos + 4
                if base + length > total:
                    break
                if block_type == 6 and length >= 32:
                    stream.seek(base)
                    def get32():
                        raw = stream.read(4)
                        return int.from_bytes(raw, "big") if len(raw) == 4 else None
                    picture_type = get32()
                    mime_len = get32()
                    if picture_type is not None and mime_len is not None and mime_len <= 512 and stream.tell() + mime_len + 4 <= base + length:
                        stream.seek(mime_len, os.SEEK_CUR)
                        desc_len = get32()
                        if desc_len is not None and desc_len <= 4096 and stream.tell() + desc_len + 20 <= base + length:
                            stream.seek(desc_len, os.SEEK_CUR)
                            width = get32()
                            height = get32()
                            _depth = get32()
                            _palette = get32()
                            data_size = get32()
                            data_offset = stream.tell()
                            if data_size is not None and 0 < data_size <= ARTWORK_MAX and data_offset + data_size <= base + length:
                                detected = _image_info(stream, data_offset, data_size)
                                if detected:
                                    fmt, detected_w, detected_h = detected
                                    w = width if width and width <= 65535 else detected_w
                                    h = height if height and height <= 65535 else detected_h
                                    ref = (2, fmt, data_offset, data_size, 0, w, h, picture_type)
                                    if winner is None or (picture_type == 3 and winner[-1] != 3):
                                        winner = ref
                                        if picture_type == 3:
                                            break
                pos = base + length
                if is_last:
                    break
            return winner
    except OSError:
        return None


def find_directory_cover(directory: Path, music_root: Path):
    # Upper/lower case names are supported without walking into other directories.
    try:
        files = {p.name.lower(): p for p in directory.iterdir() if p.is_file()}
        for basename in ("cover", "folder", "front"):
            for suffix in (".jpg", ".jpeg", ".png"):
                candidate = files.get(basename + suffix)
                if candidate is None:
                    continue
                stat = candidate.stat()
                if not (0 < stat.st_size <= ARTWORK_MAX):
                    continue
                with candidate.open("rb") as stream:
                    detected = _image_info(stream, 0, stat.st_size)
                if detected is None:
                    continue
                fmt, width, height = detected
                path = "/" + candidate.relative_to(music_root).as_posix()
                return (3, fmt, 0, stat.st_size, 0, width, height, 3, path, int(stat.st_mtime))
    except (OSError, ValueError):
        pass
    return None


def scan_artwork(path: Path, music_root: Path, fmt: int, folder_cache: Optional[dict] = None):
    # Prefer embedded type=3, else first valid picture, then directory art.
    embedded = scan_mp3(path) if fmt == 1 else scan_flac(path) if fmt == 2 else None
    if embedded is not None:
        return (*embedded, "", 0)
    directory = path.parent
    if folder_cache is not None:
        if directory not in folder_cache:
            folder_cache[directory] = find_directory_cover(directory, music_root)
        return folder_cache[directory]
    return find_directory_cover(directory, music_root)
