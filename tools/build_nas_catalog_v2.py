#!/usr/bin/env python3
"""Compatibility entry point; actual NAS builder is in tools/nas/."""
from pathlib import Path
import runpy
import sys

nas_dir = Path(__file__).resolve().parent / "nas"
sys.path.insert(0, str(nas_dir))
runpy.run_path(str(nas_dir / "build_nas_catalog_v2.py"), run_name="__main__")
