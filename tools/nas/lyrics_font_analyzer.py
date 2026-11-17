#!/usr/bin/env python3
"""FakePod lyric character scanner.

The scanner deliberately follows the current firmware lyric path:
same-directory, same-stem external synchronized LRC files. Embedded lyric tags
are not counted because the current LyricsService only loads External + Synced
LRC references. The tool is read-only with respect to the music library.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import queue
import re
import sys
import threading
import time
import unicodedata
from collections import Counter
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
import tkinter as tk

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

try:
    from build_nas_catalog_v2 import FORMAT_MAP, build_track_meta, matching_directory_lrc
    from desktop_scan_support import retry_io
except Exception as exc:  # pragma: no cover - only reached with a damaged tool folder
    raise RuntimeError("无法加载 FakePod NAS 曲库扫描模块：" + str(exc)) from exc

APP_TITLE = "FakePod · 歌词字符扫描器"
CACHE_VERSION = 1
LYRICS_MAX_BYTES = 256 * 1024
AUDIO_SUFFIXES = tuple(FORMAT_MAP.keys())
TIMESTAMP_RE = re.compile(r"^(\d+):(\d{1,2})(?:[.:](\d{1,3}))?$")
BASELINE_ASCII = "".join(chr(code) for code in range(0x20, 0x7F))


class ScanCancelled(Exception):
    pass


def normalize_visible_text(value: str) -> str:
    """Mirror TextEncoding::normalize_plain_text_codepoint for lyric display."""
    out = []
    for char in value:
        code = ord(char)
        if code in (0x00A0, 0x3000, 0x0009):
            out.append(" ")
        elif code in (0x200B, 0xFEFF):
            continue
        elif unicodedata.category(char) == "Cc":
            continue
        else:
            out.append(char)
    return "".join(out)


def decode_lrc_bytes(raw: bytes) -> str:
    """Decode the encodings accepted by the current FakePod LyricsService."""
    if not raw:
        return ""
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw.decode("utf-8-sig", errors="strict")
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        return raw.decode("utf-16", errors="strict")
    try:
        return raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        # The current firmware explicitly supports GBK in addition to UTF-8
        # and UTF-16 with BOM. Do not silently fall back to lossy replacement.
        return raw.decode("gbk", errors="strict")


def is_lrc_timestamp(tag: str) -> bool:
    match = TIMESTAMP_RE.fullmatch(tag)
    if not match:
        return False
    return int(match.group(2)) < 60


ENTITY_RE = re.compile(r"&(?:amp|lt|gt|quot|apos|nbsp|#\d+|#x[0-9A-Fa-f]+);")
NAMED_ENTITIES = {
    "&amp;": "&", "&lt;": "<", "&gt;": ">", "&quot;": '"',
    "&apos;": "'", "&nbsp;": "\u00a0",
}


def decode_supported_html_entities(value: str) -> str:
    """Match the deliberately small HTML-entity set decoded by LyricsService."""
    def replace_entity(match):
        token = match.group(0)
        if token in NAMED_ENTITIES:
            return NAMED_ENTITIES[token]
        try:
            if token.startswith(("&#x", "&#X")):
                codepoint = int(token[3:-1], 16)
            else:
                codepoint = int(token[2:-1], 10)
        except ValueError:
            return token
        if codepoint <= 0 or codepoint > 0x10FFFF or 0xD800 <= codepoint <= 0xDFFF:
            return token
        return chr(codepoint)
    return ENTITY_RE.sub(replace_entity, value)


def extract_lrc_text(decoded: str) -> list[str]:
    """Return only timestamped lyric text, matching LyricsService's LRC rules."""
    result: list[str] = []
    for raw_line in decoded.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        line = raw_line.lstrip("\ufeff").strip()
        if not line:
            continue
        cursor = 0
        has_timestamp = False
        # Firmware walks all consecutive leading [tags], skipping metadata tags
        # such as [ti:], [ar:] and [offset:] while looking for time stamps.
        while cursor < len(line) and line[cursor] == "[":
            close = line.find("]", cursor + 1)
            if close < 0:
                break
            tag = line[cursor + 1:close]
            if is_lrc_timestamp(tag):
                has_timestamp = True
            cursor = close + 1
        if not has_timestamp:
            continue
        lyric = line[cursor:].strip(" \t")
        if not lyric:
            continue
        lyric = normalize_visible_text(decode_supported_html_entities(lyric)).strip()
        if lyric:
            result.append(lyric)
    return result


def count_visible_characters(text: str) -> dict[str, int]:
    return dict(Counter(normalize_visible_text(text)))


def classify_character(char: str) -> str:
    code = ord(char)
    if code > 0xFFFF:
        return "非BMP字符"
    if char.isspace():
        return "空格/空白"
    if 0x3040 <= code <= 0x309F:
        return "日文平假名"
    if 0x30A0 <= code <= 0x30FF or 0x31F0 <= code <= 0x31FF or 0xFF66 <= code <= 0xFF9D:
        return "日文片假名"
    if 0xAC00 <= code <= 0xD7AF or 0x1100 <= code <= 0x11FF:
        return "韩文"
    if 0x3400 <= code <= 0x4DBF or 0x4E00 <= code <= 0x9FFF or 0xF900 <= code <= 0xFAFF:
        return "中文汉字"
    category = unicodedata.category(char)
    name = unicodedata.name(char, "")
    if category.startswith("N"):
        return "数字"
    if category.startswith("P"):
        return "标点符号"
    if category.startswith("S"):
        return "数学/货币/其他符号"
    if category.startswith("M"):
        return "组合附加符号"
    if "LATIN" in name:
        return "拉丁字母"
    if category.startswith("Z"):
        return "空格/空白"
    if category.startswith("C"):
        return "其他控制/未分配"
    return "其他文字"


def unicode_name(char: str) -> str:
    return unicodedata.name(char, "未命名字符")


def local_cache_path(root: Path) -> Path:
    local = os.environ.get("LOCALAPPDATA")
    base = Path(local) if local else Path.home() / ".cache"
    digest = hashlib.sha256(str(root.resolve()).casefold().encode("utf-8")).hexdigest()[:24]
    return base / "FakePod" / "lyrics-font-analyzer" / (digest + ".json")


def load_cache(path: Path, root: Path) -> dict:
    if not path.is_file():
        return {}
    try:
        if path.stat().st_size > 64 * 1024 * 1024:
            return {}
        obj = json.loads(path.read_text(encoding="utf-8"))
        if (not isinstance(obj, dict) or obj.get("version") != CACHE_VERSION or
                obj.get("root") != str(root.resolve()).casefold() or
                not isinstance(obj.get("entries"), dict)):
            return {}
        return obj["entries"]
    except (OSError, ValueError, TypeError):
        return {}


def save_cache(path: Path, root: Path, entries: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "version": CACHE_VERSION,
        "root": str(root.resolve()).casefold(),
        "updated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "entries": entries,
    }
    temp = path.with_suffix(path.suffix + ".tmp")
    with temp.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(payload, stream, ensure_ascii=False, separators=(",", ":"))
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, path)


def discover_audio_files(root: Path) -> list[Path]:
    found: list[Path] = []

    def on_error(error: OSError) -> None:
        raise error

    for dirname, dirnames, filenames in os.walk(root, onerror=on_error):
        if Path(dirname) == root:
            dirnames[:] = [name for name in dirnames if name != "_FakePod"]
        for filename in filenames:
            path = Path(dirname) / filename
            if path.suffix.lower() in AUDIO_SUFFIXES:
                found.append(path)
    found.sort(key=lambda item: item.relative_to(root).as_posix().casefold())
    return found


def _metadata_for_track(path: Path, root: Path, fmt: int) -> tuple[str, str, str]:
    try:
        meta = build_track_meta(path, root, fmt)
        return meta.title, meta.artist, meta.album
    except Exception:
        stem = path.stem
        if " - " in stem:
            artist, title = stem.split(" - ", 1)
            return title.strip(), artist.strip(), ""
        return stem, "", ""


def _entry_matches_cache(entry: dict | None, audio_stat, lrc_rel: str,
                         lrc_stat) -> bool:
    if not isinstance(entry, dict):
        return False
    if (entry.get("audio_size"), entry.get("audio_mtime_ns")) != (
            audio_stat.st_size, audio_stat.st_mtime_ns):
        return False
    if entry.get("lrc_rel", "") != lrc_rel:
        return False
    if lrc_stat is None:
        return entry.get("lrc_size", 0) == 0 and entry.get("lrc_mtime_ns", 0) == 0
    return (entry.get("lrc_size"), entry.get("lrc_mtime_ns")) == (
        lrc_stat.st_size, lrc_stat.st_mtime_ns)


def scan_library(root: Path, progress=None, cancel_event=None) -> dict:
    """Scan one Music root. Reads files only; all reports and cache stay on PC."""
    root = root.expanduser().resolve()
    if not root.is_dir():
        raise ValueError("音乐目录不存在或无法访问：" + str(root))

    cache_file = local_cache_path(root)
    old_cache = load_cache(cache_file, root)
    paths = discover_audio_files(root)
    if not paths:
        raise ValueError("没有找到 MP3、FLAC、WAV 或 OPUS 音乐文件。")
    folder_lyrics_cache: dict = {}
    new_cache: dict = {}
    tracks: list[dict] = []
    errors: list[dict] = []
    reused = 0
    scanned = 0
    total = len(paths)

    for index, audio_path in enumerate(paths, 1):
        if cancel_event is not None and cancel_event.is_set():
            raise ScanCancelled()
        relative = "/" + audio_path.relative_to(root).as_posix()
        audio_stat = audio_path.stat()
        fmt = FORMAT_MAP[audio_path.suffix.lower()]
        lrc_path = None
        lrc_rel = ""
        lrc_stat = None
        matching_error = ""
        try:
            lrc_path = matching_directory_lrc(audio_path, folder_lyrics_cache)
            if lrc_path is not None:
                lrc_stat = lrc_path.stat()
                lrc_rel = "/" + lrc_path.relative_to(root).as_posix()
        except Exception as exc:
            matching_error = str(exc)

        cached = old_cache.get(relative)
        if not matching_error and _entry_matches_cache(cached, audio_stat, lrc_rel, lrc_stat):
            entry = dict(cached)
            reused += 1
        else:
            title, artist, album = _metadata_for_track(audio_path, root, fmt)
            lyric_counts: dict[str, int] = {}
            lyric_status = "no_external_lrc"
            error_text = matching_error
            if matching_error:
                lyric_status = "ambiguous_lrc"
            elif lrc_path is not None:
                try:
                    if lrc_stat is None:
                        raise OSError("无法读取 LRC 文件属性")
                    if lrc_stat.st_size <= 0:
                        lyric_status = "empty_lrc"
                    elif lrc_stat.st_size > LYRICS_MAX_BYTES:
                        lyric_status = "oversize_lrc"
                        error_text = f"LRC 超过固件当前 256 KiB 限制：{lrc_stat.st_size} bytes"
                    else:
                        raw = retry_io(lrc_path.read_bytes)
                        after = lrc_path.stat()
                        if (after.st_size, after.st_mtime_ns) != (lrc_stat.st_size, lrc_stat.st_mtime_ns):
                            raise RuntimeError("扫描期间 LRC 文件发生变化，请重新扫描")
                        decoded = decode_lrc_bytes(raw)
                        lyric_lines = extract_lrc_text(decoded)
                        if lyric_lines:
                            lyric_counts = count_visible_characters("\n".join(lyric_lines))
                            lyric_status = "external_lrc"
                        else:
                            lyric_status = "no_timestamped_lyrics"
                            error_text = "文件存在，但没有可识别的时间戳歌词行"
                except UnicodeDecodeError as exc:
                    lyric_status = "encoding_error"
                    error_text = "歌词编码不受当前固件支持或文件编码损坏：" + str(exc)
                except Exception as exc:
                    lyric_status = "read_error"
                    error_text = str(exc)

            metadata_text = " ".join(value for value in (title, artist, album) if value)
            metadata_counts = count_visible_characters(metadata_text)
            entry = {
                "audio_size": audio_stat.st_size,
                "audio_mtime_ns": audio_stat.st_mtime_ns,
                "lrc_rel": lrc_rel,
                "lrc_size": lrc_stat.st_size if lrc_stat else 0,
                "lrc_mtime_ns": lrc_stat.st_mtime_ns if lrc_stat else 0,
                "title": title,
                "artist": artist,
                "album": album,
                "lyric_status": lyric_status,
                "error": error_text,
                "lyric_counts": lyric_counts,
                "metadata_counts": metadata_counts,
            }
            scanned += 1

        new_cache[relative] = entry
        has_lyrics = entry.get("lyric_status") == "external_lrc"
        track = {
            "path": relative,
            "title": entry.get("title", audio_path.stem),
            "artist": entry.get("artist", ""),
            "album": entry.get("album", ""),
            "lyrics_path": entry.get("lrc_rel", ""),
            "lyrics_source": entry.get("lyric_status", "unknown"),
            "lyrics_chars_unique": len(entry.get("lyric_counts", {})),
            "lyrics_chars_total": sum(entry.get("lyric_counts", {}).values()),
            "metadata_chars_unique": len(entry.get("metadata_counts", {})),
            "has_lyrics": has_lyrics,
            "error": entry.get("error", ""),
            "lyric_counts": entry.get("lyric_counts", {}),
            "metadata_counts": entry.get("metadata_counts", {}),
        }
        tracks.append(track)
        if track["error"]:
            errors.append({"path": relative, "stage": "lyrics", "error": track["error"]})

        if progress is not None and (index == 1 or index % 10 == 0 or index == total):
            progress(index, total, relative, reused, scanned)

    save_cache(cache_file, root, new_cache)
    stats: dict[str, dict] = {}
    for track in tracks:
        combined_chars = set(track["lyric_counts"]) | set(track["metadata_counts"])
        for char, count in track["lyric_counts"].items():
            row = stats.setdefault(char, {"lyric_count": 0, "metadata_count": 0,
                                          "track_count": 0, "sample_tracks": []})
            row["lyric_count"] += int(count)
        for char, count in track["metadata_counts"].items():
            row = stats.setdefault(char, {"lyric_count": 0, "metadata_count": 0,
                                          "track_count": 0, "sample_tracks": []})
            row["metadata_count"] += int(count)
        for char in combined_chars:
            row = stats.setdefault(char, {"lyric_count": 0, "metadata_count": 0,
                                          "track_count": 0, "sample_tracks": []})
            row["track_count"] += 1
            if len(row["sample_tracks"]) < 5:
                row["sample_tracks"].append(track["path"])

    lyric_tracks = sum(1 for item in tracks if item["has_lyrics"])
    no_lyric_tracks = total - lyric_tracks
    bmp_chars = {char for char in stats if ord(char) <= 0xFFFF}
    non_bmp_chars = {char for char in stats if ord(char) > 0xFFFF}
    font_chars = bmp_chars | set(BASELINE_ASCII)
    summary = {
        "tracks_total": total,
        "tracks_with_lyrics": lyric_tracks,
        "tracks_without_lyrics": no_lyric_tracks,
        "tracks_with_errors": len(errors),
        "cache_reused": reused,
        "files_rescanned": scanned,
        "unique_lyric_chars": len({c for t in tracks for c in t["lyric_counts"]}),
        "unique_metadata_chars": len({c for t in tracks for c in t["metadata_counts"]}),
        "unique_all_chars": len(stats),
        "unique_font_chars_bmp": len(font_chars),
        "unsupported_non_bmp_chars": len(non_bmp_chars),
    }
    return {
        "root": str(root),
        "summary": summary,
        "tracks": tracks,
        "character_stats": stats,
        "errors": errors,
        "baseline_ascii": BASELINE_ASCII,
        "cache_file": str(cache_file),
    }


def _csv_write(path: Path, fieldnames: list[str], rows) -> None:
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def export_results(result: dict, output_dir: Path) -> list[Path]:
    output_dir = output_dir.expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    stats = result["character_stats"]
    all_chars = sorted(stats, key=ord)
    bmp_chars = sorted((char for char in all_chars if ord(char) <= 0xFFFF), key=ord)
    non_bmp_chars = sorted((char for char in all_chars if ord(char) > 0xFFFF), key=ord)
    font_chars = sorted(set(bmp_chars) | set(BASELINE_ASCII), key=ord)
    charset_text = "".join(font_chars)
    all_charset_text = "".join(all_chars)

    outputs: list[Path] = []
    for name in ("lyrics_charset_24.txt", "lyrics_charset_32.txt", "lyrics_charset_bmp.txt"):
        path = output_dir / name
        path.write_text(charset_text, encoding="utf-8")
        outputs.append(path)
    all_path = output_dir / "lyrics_charset_all.txt"
    all_path.write_text(all_charset_text, encoding="utf-8")
    outputs.append(all_path)

    lines_path = output_dir / "lyrics_charset_codepoints.txt"
    with lines_path.open("w", encoding="utf-8-sig", newline="\n") as stream:
        stream.write("FakePod 字符集（按 Unicode 码点排序）\n")
        for char in font_chars:
            stream.write(f"U+{ord(char):04X}\t{char}\t{classify_character(char)}\t{unicode_name(char)}\n")
    outputs.append(lines_path)

    non_bmp_path = output_dir / "unsupported_non_bmp.txt"
    non_bmp_path.write_text(
        "".join(f"U+{ord(char):04X}\t{char}\t{unicode_name(char)}\n" for char in non_bmp_chars),
        encoding="utf-8",
    )
    outputs.append(non_bmp_path)

    stat_rows = []
    for char in all_chars:
        row = stats[char]
        stat_rows.append({
            "codepoint": f"U+{ord(char):04X}",
            "character": char,
            "category": classify_character(char),
            "unicode_name": unicode_name(char),
            "lyric_count": row["lyric_count"],
            "metadata_count": row["metadata_count"],
            "total_count": row["lyric_count"] + row["metadata_count"],
            "track_count": row["track_count"],
            "supported_by_current_font_bin": ord(char) <= 0xFFFF,
            "sample_tracks": " | ".join(row["sample_tracks"]),
        })
    stat_rows.sort(key=lambda row: (-row["total_count"], int(row["codepoint"][2:], 16)))
    stat_path = output_dir / "character_statistics.csv"
    _csv_write(stat_path, ["codepoint", "character", "category", "unicode_name",
                           "lyric_count", "metadata_count", "total_count", "track_count",
                           "supported_by_current_font_bin", "sample_tracks"], stat_rows)
    outputs.append(stat_path)

    track_rows = []
    for track in result["tracks"]:
        track_rows.append({
            "path": track["path"],
            "title": track["title"],
            "artist": track["artist"],
            "album": track["album"],
            "lyrics_source": track["lyrics_source"],
            "lyrics_path": track["lyrics_path"],
            "lyrics_chars_unique": track["lyrics_chars_unique"],
            "lyrics_chars_total": track["lyrics_chars_total"],
            "metadata_chars_unique": track["metadata_chars_unique"],
            "error": track["error"],
        })
    track_path = output_dir / "track_lyrics_report.csv"
    _csv_write(track_path, ["path", "title", "artist", "album", "lyrics_source",
                           "lyrics_path", "lyrics_chars_unique", "lyrics_chars_total",
                           "metadata_chars_unique", "error"], track_rows)
    outputs.append(track_path)

    missing_path = output_dir / "tracks_without_lyrics.csv"
    missing_rows = [row for row in track_rows if row["lyrics_source"] != "external_lrc"]
    _csv_write(missing_path, ["path", "title", "artist", "lyrics_source", "lyrics_path", "error"],
               missing_rows)
    outputs.append(missing_path)

    error_path = output_dir / "scan_errors.csv"
    _csv_write(error_path, ["path", "stage", "error"], result["errors"])
    outputs.append(error_path)

    manifest = {
        "tool": "FakePod Lyrics Font Analyzer",
        "schema_version": 1,
        "source_root": result["root"],
        "generated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "summary": result["summary"],
        "font_sizes_px": [24, 32],
        "font_charsets_identical": True,
        "font_charset_files": ["lyrics_charset_24.txt", "lyrics_charset_32.txt"],
        "font_charset_encoding": "UTF-8",
        "font_charset_order": "Unicode code point ascending",
        "current_firmware_lyrics_source": "same-directory same-stem external synchronized LRC",
        "current_firmware_lrc_max_bytes": LYRICS_MAX_BYTES,
        "current_font_bin_codepoint_limit": "BMP only (U+0000..U+FFFF)",
        "non_bmp_characters_are_separated": True,
        "cache_file": result["cache_file"],
    }
    manifest_path = output_dir / "font_manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    outputs.append(manifest_path)
    return outputs


class AnalyzerApp:
    def __init__(self, window: tk.Tk, initial_root: str = ""):
        self.window = window
        self.window.title(APP_TITLE)
        self.window.geometry("940x760")
        self.window.minsize(780, 650)
        self.root_var = tk.StringVar(value=initial_root)
        default_output = Path.home() / "Documents" / "FakePod_Font_Exports"
        self.output_var = tk.StringVar(value=str(default_output))
        self.events: queue.Queue = queue.Queue()
        self.cancel_event = threading.Event()
        self.busy = False
        self.result: dict | None = None
        self._build()
        self.window.protocol("WM_DELETE_WINDOW", self._on_close)
        self.window.after(100, self._poll_events)

    def _build(self):
        main = ttk.Frame(self.window, padding=14)
        main.pack(fill="both", expand=True)
        ttk.Label(main, text="FakePod 歌词字符扫描与字库清单",
                  font=("Microsoft YaHei UI", 16, "bold")).pack(anchor="w")
        ttk.Label(main, text="扫描 Music 根目录中的外置同步 LRC，统计歌词、歌曲标题和歌手实际需要的 Unicode 字符。",
                  foreground="#555").pack(anchor="w", pady=(4, 10))

        source = ttk.LabelFrame(main, text="1. 音乐目录", padding=10)
        source.pack(fill="x", pady=4)
        row = ttk.Frame(source)
        row.pack(fill="x")
        ttk.Entry(row, textvariable=self.root_var).pack(side="left", fill="x", expand=True)
        self.browse_root_button = ttk.Button(row, text="选择目录…", command=self._browse_root)
        self.browse_root_button.pack(side="left", padx=(8, 0))

        output = ttk.LabelFrame(main, text="2. 导出目录（只写入电脑，不修改 NAS 音乐目录）", padding=10)
        output.pack(fill="x", pady=4)
        row = ttk.Frame(output)
        row.pack(fill="x")
        ttk.Entry(row, textvariable=self.output_var).pack(side="left", fill="x", expand=True)
        self.browse_output_button = ttk.Button(row, text="选择目录…", command=self._browse_output)
        self.browse_output_button.pack(side="left", padx=(8, 0))

        actions = ttk.Frame(main)
        actions.pack(fill="x", pady=(8, 5))
        self.scan_button = ttk.Button(actions, text="开始扫描全部歌曲", command=self._start_scan)
        self.scan_button.pack(side="left")
        self.cancel_button = ttk.Button(actions, text="取消扫描", command=self._cancel_scan, state="disabled")
        self.cancel_button.pack(side="left", padx=8)
        self.export_button = ttk.Button(actions, text="导出字符集与报告", command=self._export,
                                        state="disabled")
        self.export_button.pack(side="left")

        summary = ttk.LabelFrame(main, text="3. 扫描结果", padding=10)
        summary.pack(fill="x", pady=4)
        self.summary_label = ttk.Label(summary, text="尚未扫描", justify="left")
        self.summary_label.pack(anchor="w")
        self.progress = ttk.Progressbar(main, mode="determinate", maximum=100)
        self.progress.pack(fill="x", pady=(10, 3))
        self.progress_label = ttk.Label(main, text="等待扫描", foreground="#24587a")
        self.progress_label.pack(anchor="w")

        logframe = ttk.LabelFrame(main, text="扫描日志", padding=8)
        logframe.pack(fill="both", expand=True, pady=(8, 0))
        self.logbox = tk.Text(logframe, wrap="word", state="disabled",
                              font=("Microsoft YaHei UI", 9), height=12)
        self.logbox.pack(fill="both", expand=True)
        self._log("就绪。当前版本按 FakePod 固件实际读取规则扫描外置同步 LRC；不扫描内嵌歌词标签。")
        self._log("首次扫描会读取歌词；后续会依据歌曲/LRC 的大小和修改时间复用本机缓存。")

    def _log(self, message: str):
        self.logbox.configure(state="normal")
        self.logbox.insert("end", str(message).rstrip() + "\n")
        self.logbox.see("end")
        self.logbox.configure(state="disabled")

    def _browse_root(self):
        selected = filedialog.askdirectory(title="选择 FakePod Music 根目录",
                                           initialdir=self.root_var.get() or str(Path.home()))
        if selected:
            self.root_var.set(selected)

    def _browse_output(self):
        selected = filedialog.askdirectory(title="选择字符集导出目录",
                                           initialdir=self.output_var.get() or str(Path.home()))
        if selected:
            self.output_var.set(selected)

    def _set_busy(self, busy: bool):
        self.busy = busy
        self.scan_button.configure(state="disabled" if busy else "normal")
        self.browse_root_button.configure(state="disabled" if busy else "normal")
        self.browse_output_button.configure(state="disabled" if busy else "normal")
        self.cancel_button.configure(state="normal" if busy else "disabled")
        self.export_button.configure(state="disabled" if busy or self.result is None else "normal")

    def _start_scan(self):
        if self.busy:
            return
        root = Path(self.root_var.get().strip())
        if not root.is_dir():
            messagebox.showerror("目录无效", "请选择可访问的 Music 音乐根目录。")
            return
        self.result = None
        self.export_button.configure(state="disabled")
        self.cancel_event.clear()
        self._set_busy(True)
        self.progress.configure(maximum=1, value=0)
        self.progress_label.configure(text="正在统计音乐文件…")
        self.summary_label.configure(text="扫描中…")
        self._log("开始扫描：" + str(root))

        def report(done, total, path, reused, scanned):
            self.events.put(("progress", (done, total, path, reused, scanned)))

        def worker():
            try:
                result = scan_library(root, report, self.cancel_event)
                self.events.put(("complete", (result,)))
            except ScanCancelled:
                self.events.put(("cancelled", ()))
            except Exception as exc:
                self.events.put(("error", (str(exc),)))

        threading.Thread(target=worker, daemon=True).start()

    def _cancel_scan(self):
        if self.busy:
            self.cancel_event.set()
            self.cancel_button.configure(state="disabled")
            self.progress_label.configure(text="正在取消…")

    def _export(self):
        if self.result is None:
            return
        output = Path(self.output_var.get().strip())
        if not str(output).strip():
            messagebox.showerror("导出目录为空", "请选择导出目录。")
            return
        try:
            files = export_results(self.result, output)
        except Exception as exc:
            messagebox.showerror("导出失败", str(exc))
            return
        self._log("导出完成：" + str(output))
        self._log("生成文件：" + "、".join(path.name for path in files))
        messagebox.showinfo("导出完成", f"已生成 {len(files)} 个文件：\n{output}")

    def _poll_events(self):
        try:
            for _ in range(100):
                event, args = self.events.get_nowait()
                if event == "progress":
                    done, total, path, reused, scanned = args
                    self.progress.configure(maximum=max(1, total), value=done)
                    self.progress_label.configure(
                        text=f"{done}/{total} 首 · {done * 100 // max(1, total)}% · 缓存复用 {reused} · 本次读取 {scanned}\n{path}")
                elif event == "complete":
                    self.result = args[0]
                    self._set_busy(False)
                    summary = self.result["summary"]
                    self.summary_label.configure(text=(
                        f"歌曲总数：{summary['tracks_total']}    找到可用 LRC：{summary['tracks_with_lyrics']}    "
                        f"没有可用 LRC：{summary['tracks_without_lyrics']}\n"
                        f"歌词不重复字符：{summary['unique_lyric_chars']}    标题/歌手/专辑字符：{summary['unique_metadata_chars']}    "
                        f"合计：{summary['unique_all_chars']}\n"
                        f"当前字体格式可用 BMP 字符：{summary['unique_font_chars_bmp']}    "
                        f"非 BMP 字符（当前 .bin 格式不支持）：{summary['unsupported_non_bmp_chars']}\n"
                        f"缓存复用：{summary['cache_reused']}    本次重新读取：{summary['files_rescanned']}    "
                        f"异常：{summary['tracks_with_errors']}"))
                    self.progress.configure(maximum=max(1, summary["tracks_total"]),
                                            value=summary["tracks_total"])
                    self.progress_label.configure(text="扫描完成。可以导出字符集和报告。")
                    self._log("扫描完成：" + json.dumps(summary, ensure_ascii=False))
                    self.export_button.configure(state="normal")
                elif event == "cancelled":
                    self._set_busy(False)
                    self.progress_label.configure(text="扫描已取消；未保存本轮缓存。")
                    self._log("扫描已取消。")
                elif event == "error":
                    self._set_busy(False)
                    self.progress_label.configure(text="扫描失败")
                    self._log("错误：" + args[0])
                    messagebox.showerror("扫描失败", args[0])
        except queue.Empty:
            pass
        if self.window.winfo_exists():
            self.window.after(100, self._poll_events)

    def _on_close(self):
        if self.busy:
            self.cancel_event.set()
        self.window.destroy()


def main():
    parser = argparse.ArgumentParser(description="FakePod 歌词字符扫描器")
    parser.add_argument("--root", default="", help="Music 根目录；省略时由界面选择")
    parser.add_argument("--out", default="", help="导出目录；指定后自动扫描并导出")
    parser.add_argument("--no-gui", action="store_true", help="命令行扫描，不启动 Tk 界面")
    args = parser.parse_args()

    if args.no_gui:
        if not args.root:
            parser.error("--no-gui 必须同时指定 --root")
        root = Path(args.root)
        result = scan_library(root, lambda done, total, path, reused, scanned:
                              print(f"扫描进度 {done}/{total} 缓存复用={reused} 本次读取={scanned} {path}", flush=True))
        output = Path(args.out) if args.out else Path.cwd() / "FakePod_Font_Exports"
        files = export_results(result, output)
        print(json.dumps(result["summary"], ensure_ascii=False, indent=2))
        print("导出目录：" + str(output.resolve()))
        print("文件：" + ", ".join(path.name for path in files))
        return

    window = tk.Tk()
    AnalyzerApp(window, args.root)
    window.mainloop()
    if args.out:
        # GUI mode intentionally leaves export to the user's explicit button.
        pass


if __name__ == "__main__":
    main()
