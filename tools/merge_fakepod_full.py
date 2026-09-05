#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""将 PlatformIO 生成的 4 个 ESP32-S3 固件镜像合并为可从 0x0000 烧录的完整镜像。"""

from __future__ import annotations

import argparse
import csv
import hashlib
import shlex
import sys
from pathlib import Path

DEFAULT_ENV = "fakepod_nano"
OUTPUT_NAME = "FakePod_Full.bin"


def parse_int(value: str) -> int:
    return int(value.strip(), 0)


def parse_partition_offsets(partitions_csv: Path) -> dict[str, int]:
    offsets: dict[str, int] = {}
    with partitions_csv.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.reader(handle):
            if not row or row[0].lstrip().startswith("#") or len(row) < 4:
                continue
            name = row[0].strip()
            offset = row[3].strip()
            if name and offset:
                offsets[name] = parse_int(offset)
    return offsets


def parse_flash_args(build_dir: Path) -> dict[str, int]:
    """优先读取 PlatformIO/ESP-IDF 生成的实际烧录参数，避免硬编码偏移。"""
    wanted = {
        "bootloader.bin",
        "partitions.bin",
        "ota_data_initial.bin",
        "firmware.bin",
    }
    result: dict[str, int] = {}

    for name in ("flash_args", "flash_project_args"):
        path = build_dir / name
        if not path.is_file():
            continue

        try:
            tokens = shlex.split(path.read_text(encoding="utf-8", errors="ignore"))
        except OSError:
            continue

        for index in range(len(tokens) - 1):
            token = tokens[index]
            image = Path(tokens[index + 1]).name
            if image not in wanted:
                continue
            try:
                result[image] = parse_int(token)
            except ValueError:
                continue

        if wanted.issubset(result):
            return result

    return result


def resolve_offsets(project_root: Path, build_dir: Path) -> tuple[dict[str, int], str]:
    flash_offsets = parse_flash_args(build_dir)
    required = {
        "bootloader.bin",
        "partitions.bin",
        "ota_data_initial.bin",
        "firmware.bin",
    }
    if required.issubset(flash_offsets):
        return flash_offsets, "PlatformIO flash_args"

    partitions_csv = project_root / "partitions.csv"
    if not partitions_csv.is_file():
        raise FileNotFoundError(f"找不到分区表：{partitions_csv}")

    partition_offsets = parse_partition_offsets(partitions_csv)
    if "otadata" not in partition_offsets or "app0" not in partition_offsets:
        raise ValueError("partitions.csv 缺少 otadata 或 app0，无法安全确定合并偏移")

    # 当前工程为 ESP32-S3 + ESP-IDF 默认启动布局：Bootloader=0x0000，分区表=0x8000。
    # otadata 与 app0 则直接从当前项目 partitions.csv 读取，避免与工程配置漂移。
    offsets = {
        "bootloader.bin": 0x0000,
        "partitions.bin": 0x8000,
        "ota_data_initial.bin": partition_offsets["otadata"],
        "firmware.bin": partition_offsets["app0"],
    }
    return offsets, "partitions.csv + ESP32-S3 默认启动偏移"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def merge_images(build_dir: Path, output_path: Path, offsets: dict[str, int]) -> None:
    images: list[tuple[int, Path, int]] = []
    for image_name, offset in offsets.items():
        image_path = build_dir / image_name
        if not image_path.is_file():
            raise FileNotFoundError(f"缺少编译产物：{image_path}")
        images.append((offset, image_path, image_path.stat().st_size))

    images.sort(key=lambda item: item[0])

    previous_end = 0
    for offset, path, size in images:
        if offset < previous_end:
            raise ValueError(
                f"镜像发生地址重叠：{path.name} 起始=0x{offset:X}，前一镜像结束=0x{previous_end:X}"
            )
        previous_end = offset + size

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("wb") as output:
        current = 0
        ff_block = b"\xFF" * 65536

        for offset, image_path, _ in images:
            gap = offset - current
            while gap > 0:
                chunk_size = min(gap, len(ff_block))
                output.write(ff_block[:chunk_size])
                gap -= chunk_size
                current += chunk_size

            with image_path.open("rb") as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    output.write(chunk)
                    current += len(chunk)

    print("FakePod 完整固件合并完成")
    print(f"输出文件：{output_path}")
    print()
    for offset, image_path, size in images:
        print(
            f"  0x{offset:06X}  {image_path.name:<22} "
            f"{size:>9,} B  结束=0x{offset + size:06X}"
        )
    print()
    print(f"合并后大小：{output_path.stat().st_size:,} B (0x{output_path.stat().st_size:X})")
    print(f"SHA256：{sha256_file(output_path)}")
    print("烧录起始地址：0x0000")


def main() -> int:
    script_path = Path(__file__).resolve()
    project_root = script_path.parent.parent

    parser = argparse.ArgumentParser(description="合并 FakePod ESP32-S3 完整固件")
    parser.add_argument("--env", default=DEFAULT_ENV, help=f"PlatformIO 环境名，默认 {DEFAULT_ENV}")
    parser.add_argument(
        "--build-dir",
        type=Path,
        help="编译输出目录，默认 .pio/build/<env>",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help=f"输出文件，默认写入项目根目录/{OUTPUT_NAME}",
    )
    args = parser.parse_args()

    build_dir = (args.build_dir or (project_root / ".pio" / "build" / args.env)).resolve()
    output_path = (args.output or (project_root / OUTPUT_NAME)).resolve()

    try:
        offsets, source = resolve_offsets(project_root, build_dir)
        print(f"偏移来源：{source}")
        merge_images(build_dir, output_path, offsets)
    except (FileNotFoundError, ValueError, OSError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
