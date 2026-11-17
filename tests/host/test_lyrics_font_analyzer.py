#!/usr/bin/env python3
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "nas"))

import lyrics_font_analyzer as analyzer


class LyricsFontAnalyzerTests(unittest.TestCase):
    def test_lrc_parser_uses_only_timestamped_lines(self):
        source = (
            "[ti:晴天]\n"
            "[ar:周杰伦]\n"
            "[offset:-120]\n"
            "[00:01.00][00:02.00]你好，世界！\n"
            "[00:03.5]I &amp; you &lt;3\n"
            "[00:99.00]invalid seconds\n"
            "not a timestamped line\n"
        )
        self.assertEqual(
            analyzer.extract_lrc_text(source),
            ["你好，世界！", "I & you <3"],
        )

    def test_firmware_supported_encodings(self):
        text = "[00:01.00]你好"
        self.assertEqual(analyzer.decode_lrc_bytes(b"\xef\xbb\xbf" + text.encode("utf-8")), text)
        self.assertEqual(analyzer.decode_lrc_bytes(text.encode("utf-16")), text)
        self.assertEqual(analyzer.decode_lrc_bytes(text.encode("gbk")), text)

    def test_only_firmware_supported_html_entities_are_decoded(self):
        text = analyzer.decode_supported_html_entities(
            "&amp; &lt; &#20320; &#x597D; &eacute;"
        )
        self.assertEqual(text, "& < 你 好 &eacute;")

    def test_whitespace_normalization_matches_firmware(self):
        self.assertEqual(
            analyzer.normalize_visible_text("A\u00a0B\u3000C\u200bD\ufeff\tE"),
            "A B CDE E",
        )

    def test_character_categories_and_bmp_limit(self):
        self.assertEqual(analyzer.classify_character("你"), "中文汉字")
        self.assertEqual(analyzer.classify_character("あ"), "日文平假名")
        self.assertEqual(analyzer.classify_character("ア"), "日文片假名")
        self.assertEqual(analyzer.classify_character("한"), "韩文")
        self.assertEqual(analyzer.classify_character("A"), "拉丁字母")
        self.assertEqual(analyzer.classify_character("😀"), "非BMP字符")

    def test_export_produces_identical_24_and_32_charsets(self):
        result = {
            "root": "C:/Music",
            "summary": {
                "tracks_total": 1,
                "tracks_with_lyrics": 1,
                "tracks_without_lyrics": 0,
                "tracks_with_errors": 0,
                "cache_reused": 0,
                "files_rescanned": 1,
                "unique_lyric_chars": 2,
                "unique_metadata_chars": 1,
                "unique_all_chars": 3,
                "unique_font_chars_bmp": 98,
                "unsupported_non_bmp_chars": 1,
            },
            "tracks": [{
                "path": "/Artist/Song.mp3",
                "title": "Song",
                "artist": "Artist",
                "album": "",
                "lyrics_path": "/Artist/Song.lrc",
                "lyrics_source": "external_lrc",
                "lyrics_chars_unique": 2,
                "lyrics_chars_total": 3,
                "metadata_chars_unique": 1,
                "has_lyrics": True,
                "error": "",
                "lyric_counts": {"你": 2, "😀": 1},
                "metadata_counts": {"S": 1},
            }],
            "character_stats": {
                "你": {"lyric_count": 2, "metadata_count": 0, "track_count": 1,
                       "sample_tracks": ["/Artist/Song.mp3"]},
                "😀": {"lyric_count": 1, "metadata_count": 0, "track_count": 1,
                       "sample_tracks": ["/Artist/Song.mp3"]},
                "S": {"lyric_count": 0, "metadata_count": 1, "track_count": 1,
                      "sample_tracks": ["/Artist/Song.mp3"]},
            },
            "errors": [],
            "cache_file": "local-cache.json",
        }
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            files = analyzer.export_results(result, output)
            self.assertEqual(len(files), 11)
            charset24 = (output / "lyrics_charset_24.txt").read_text(encoding="utf-8")
            charset32 = (output / "lyrics_charset_32.txt").read_text(encoding="utf-8")
            self.assertEqual(charset24, charset32)
            self.assertIn("你", charset24)
            self.assertIn("S", charset24)
            self.assertNotIn("😀", charset24)
            self.assertIn("😀", (output / "unsupported_non_bmp.txt").read_text(encoding="utf-8"))
            self.assertTrue((output / "character_statistics.csv").is_file())
            self.assertTrue((output / "tracks_without_lyrics.csv").is_file())
            self.assertTrue((output / "font_manifest.json").is_file())


if __name__ == "__main__":
    unittest.main()
