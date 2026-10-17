# R145.16 — NAS FLAC bounded frame scan with same-connection replay

Based on R145.15.

- Reuse cached SEEKTABLE to choose a bounded byte window around the target using adjacent sample/byte anchors. Interpolation selects only the scan window; it is never treated as the final frame location.
- Read the window sequentially from one HTTP Range connection and scan FLAC frame headers with the existing header CRC8 parser.
- Stop reading after a valid frame header beyond the target when possible; keep a 32-byte tail between incremental scans so split headers are covered.
- Select the latest valid frame at/before target only when it is within two STREAMINFO maximum blocks of the target.
- Transfer the bytes from the selected frame through the scanned suffix into the existing HTTP source replay buffer. After replay drains, the same HTTP ring continues, avoiding a second Range handshake.
- If refinement fails, close the consumed scan stream and fall back to the original SEEKTABLE anchor via a fresh Range request.
- CBR/Xing/VBRI MP3 paths and local FLAC seek logic are unchanged.

Validation: source-level API/call-site consistency checks, patch whitespace check with CRLF-aware Git settings, and clean-baseline patch application check. No ESP-IDF/PlatformIO build or device test was run.
