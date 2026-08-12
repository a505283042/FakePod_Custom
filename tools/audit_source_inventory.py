#!/usr/bin/env python3
"""R.37 source inventory helper.

Pure source audit: counts C/C++ LOC and ESP_LOG calls by module, then reports
legacy display symbols and R.37 interface-boundary regressions. It never modifies the project.
"""

from __future__ import annotations

from collections import defaultdict
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
CODE_SUFFIXES = {".c", ".cpp", ".h", ".hpp"}
LOG_RE = re.compile(r"ESP_LOG[EWIDV]\s*\(")
EMPTY_DISABLED_LOG_RE = re.compile(r"#define\s+\w*LOGI\(\.\.\.\)\s+do\s*\{\s*\}\s*while\s*\(0\)")
FORBIDDEN_LEGACY_PATTERNS = {
    "display_present_rgb565_direct": re.compile(r"\bdisplay_present_rgb565_direct\b"),
    "display_present_rgb565_region_direct": re.compile(r"\bdisplay_present_rgb565_region_direct\b"),
    "display_present_rgb565_region_direct_owned": re.compile(r"\bdisplay_present_rgb565_region_direct_owned\b"),
    "kLauncherDirectSceneEnabled": re.compile(r"\bkLauncherDirectSceneEnabled\b"),
    "kFullscreenHomeResumeDirectEnabled": re.compile(r"\bkFullscreenHomeResumeDirectEnabled\b"),
    "g_launcher_direct_frame_active": re.compile(r"\bg_launcher_direct_frame_active\b"),
    "player_home_launcher_begin_direct_scene": re.compile(r"\bplayer_home_launcher_begin_direct_scene\b"),
    "player_home_launcher_present_work_direct": re.compile(r"\bplayer_home_launcher_present_work_direct\b"),
    "now_playing_artwork_set_direct_present_allowed": re.compile(r"\bnow_playing_artwork_set_direct_present_allowed\b"),
    "now_playing_artwork_take_direct_present_event": re.compile(r"\bnow_playing_artwork_take_direct_present_event\b"),
}

BOUNDED_HEADER = SRC / "drivers" / "display" / "display_bounded_spi.h"
BOUNDED_HEADER_ALLOWED_INCLUDES = {
    Path("drivers/display/display_bounded_spi.cpp"),
    Path("ui/artwork/now_playing_artwork.cpp"),
    Path("ui/screens/player_home.cpp"),
}
BACKEND_HEADER_ALLOWED_INCLUDES = {
    Path("drivers/display/display.cpp"),
    Path("drivers/display/display_bounded_spi.cpp"),
    Path("ui/ui_manager.cpp"),
}
BOUNDARY_FORBIDDEN_IN_DISPLAY_H = re.compile(
    r"DisplayBoundedSpi|display_(?:launcher|cover)_bounded_spi|esp_lcd_panel|display_te_|display_install_lvgl"
)



def main() -> int:
    modules: dict[str, list[int]] = defaultdict(lambda: [0, 0, 0])
    total_files = 0
    total_loc = 0
    total_logs = 0
    heavy: list[tuple[int, int, Path]] = []
    legacy_hits: list[tuple[str, Path, int, str]] = []
    empty_disabled_log_hits: list[tuple[Path, int, str]] = []
    boundary_hits: list[tuple[Path, int, str]] = []

    for path in sorted(SRC.rglob("*")):
        if not path.is_file() or path.suffix not in CODE_SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        lines = text.splitlines()
        loc = len(lines)
        logs = len(LOG_RE.findall(text))
        rel = path.relative_to(SRC)
        module = rel.parts[0] if len(rel.parts) > 1 else "(root)"

        total_files += 1
        total_loc += loc
        total_logs += logs
        modules[module][0] += 1
        modules[module][1] += loc
        modules[module][2] += logs
        heavy.append((logs, loc, rel))

        for line_no, line in enumerate(lines, 1):
            if rel == Path("drivers/display/display.h") and BOUNDARY_FORBIDDEN_IN_DISPLAY_H.search(line):
                boundary_hits.append((rel, line_no, line.strip()))
            if '#include "display_bounded_spi.h"' in line and rel not in BOUNDED_HEADER_ALLOWED_INCLUDES:
                boundary_hits.append((rel, line_no, line.strip()))
            if '#include "display_backend.h"' in line and rel not in BACKEND_HEADER_ALLOWED_INCLUDES:
                boundary_hits.append((rel, line_no, line.strip()))
            if EMPTY_DISABLED_LOG_RE.search(line):
                empty_disabled_log_hits.append((rel, line_no, line.strip()))
            for name, pattern in FORBIDDEN_LEGACY_PATTERNS.items():
                if pattern.search(line):
                    legacy_hits.append((name, rel, line_no, line.strip()))

    print(f"C/C++ files : {total_files}")
    print(f"LOC         : {total_loc}")
    print(f"ESP_LOG*    : {total_logs}")
    print("\nBy module:")
    for module, values in sorted(modules.items()):
        print(f"  {module:12s} files={values[0]:3d} loc={values[1]:6d} logs={values[2]:4d}")

    print("\nTop log files:")
    for logs, loc, rel in sorted(heavy, reverse=True)[:15]:
        print(f"  logs={logs:3d} loc={loc:5d} {rel.as_posix()}")

    print("\nEmpty disabled LOGI macros:")
    if empty_disabled_log_hits:
        for rel, line_no, line in empty_disabled_log_hits:
            print(f"  {rel}:{line_no}: {line}")
    else:
        print("  none")

    print("\nDisplay API boundary violations:")
    if boundary_hits:
        for rel, line_no, line in boundary_hits:
            print(f"  {rel}:{line_no}: {line}")
    else:
        print("  none")

    print("\nForbidden legacy display symbols:")
    if legacy_hits:
        for name, rel, line_no, line in legacy_hits:
            print(f"  {name}: {rel}:{line_no}: {line}")
        return 2
    if boundary_hits:
        return 4
    if empty_disabled_log_hits:
        return 3
    print("  none")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
