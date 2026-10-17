from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
flac_h = (ROOT / "src/audio/decoders/flac_decoder.h").read_text(encoding="utf-8")
flac_cpp = (ROOT / "src/audio/decoders/flac_decoder.cpp").read_text(encoding="utf-8")
http_h = (ROOT / "src/audio/sources/buffered_http_audio_source.h").read_text(encoding="utf-8")
http_cpp = (ROOT / "src/audio/sources/buffered_http_audio_source.cpp").read_text(encoding="utf-8")
service = (ROOT / "src/audio/audio_service.cpp").read_text(encoding="utf-8")

assert "flac_decoder_refine_nas_seek_range" in flac_h
assert flac_cpp.count("esp_err_t flac_decoder_refine_nas_seek_range(") == 1
assert "buffered_http_audio_source_attach_replay" in http_h
assert http_cpp.count("esp_err_t buffered_http_audio_source_attach_replay(") == 1
assert "flac_decoder_refine_nas_seek_range(" in service
assert "buffered_http_audio_source_attach_replay(" in service
assert "kMaxScanBytes = 512U * 1024U" in flac_cpp
assert "max_gap" in flac_cpp and "flac_parse_frame_sample_number" in flac_cpp
assert "connection_reused=1" in service
assert "NAS_FLAC_SEEK_REFINE" in flac_cpp and "fallback to SEEKTABLE" in service
print("PASS R145.16 static wiring: cached plan, bounded FLAC frame scan, same-connection replay ownership, and fallback path")
